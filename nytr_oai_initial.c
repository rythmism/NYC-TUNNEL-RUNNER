/*******************************************************************************************
*
*   NYC TUNNEL RUNNER
*   An endless runner set in a New York City subway tunnel.
*
*   - Pick a cosmetic outfit for your commuter (purely visual, no gameplay effect)
*   - Jump potholes & steam vents, duck falling debris & low beams
*   - Collect coins (common) and paper money (rare, worth more)
*   - Game speeds up the longer you survive
*   - Procedurally generated 8-bit style square-wave chiptune (original klezmer-scale
*     melody -- not a reproduction of any copyrighted recording/arrangement)
*
*   Build: gcc main.c -o nyc_tunnel_runner -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
*   (see README.md for platform-specific instructions)
*
********************************************************************************************/

#include "raylib.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <time.h>

#define SCREEN_W 960
#define SCREEN_H 540
#define GROUND_Y 420
#define MAX_OBSTACLES 24
#define MAX_COLLECTIBLES 24
#define MAX_PARTICLES 64

#ifndef PI
#define PI 3.14159265358979323846f
#endif

/* ---------------------------------------------------------------------------------------
   TYPES
--------------------------------------------------------------------------------------- */

typedef enum { OUTFIT_STREETWEAR = 0, OUTFIT_MTA, OUTFIT_TOURIST, OUTFIT_HASIDIC, OUTFIT_COUNT } OutfitType;
typedef enum { OBS_POTHOLE, OBS_VENT, OBS_DEBRIS, OBS_BEAM } ObstacleType;
typedef enum { COL_COIN, COL_BILL } CollectibleType;
typedef enum { SCR_MENU, SCR_PLAYING, SCR_GAMEOVER } GameScreen;
typedef enum { SIZE_SMALL = 0, SIZE_MEDIUM, SIZE_LARGE } SizeTier;

typedef struct {
    Rectangle rect;
    ObstacleType type;
    SizeTier tier;
    bool active;
    bool scored; /* passed safely, for near-miss feedback (unused for scoring but kept clean) */
} Obstacle;

typedef struct {
    Rectangle rect;
    CollectibleType type;
    bool active;
    int value;
} Collectible;

typedef struct {
    Vector2 pos;
    Vector2 vel;
    float life;
    Color color;
    bool active;
} Particle;

typedef struct {
    float x;
    float y;
    float velY;
    bool isJumping;
    bool isDucking;
    float invulnTimer;
    Rectangle rect;
} Player;

/* ---------------------------------------------------------------------------------------
   GLOBAL STATE
--------------------------------------------------------------------------------------- */

static Obstacle obstacles[MAX_OBSTACLES];
static Collectible collectibles[MAX_COLLECTIBLES];
static Particle particles[MAX_PARTICLES];

static Player player;
static GameScreen screen = SCR_MENU;
static OutfitType selectedOutfit = OUTFIT_STREETWEAR;

static float gameTime = 0.0f;
static float gameSpeed = 320.0f;
static float spawnTimer = 0.0f;
static float collectibleSpawnTimer = 0.0f;
static float bgScroll = 0.0f;
static float cityScroll = 0.0f;
static float screenShake = 0.0f;
static float hitFlash = 0.0f;
static float uiPulse = 0.0f;

static int score = 0;
static int coinsCollected = 0;
static int billsCollected = 0;
static float health = 100.0f;

static int bestScore = 0;

/* Audio */
static Sound sfxJump;
static Sound sfxCoin;
static Sound sfxBill;
static Sound sfxHit;
static Sound sfxGameOver;

#define MELODY_LEN 16
static Sound melodyNotes[MELODY_LEN];
static int melodyIndex = 0;
static float melodyTimer = 0.0f;
static const float NOTE_DURATION = 0.18f;

/* ---------------------------------------------------------------------------------------
   AUDIO GENERATION (procedural, no external files needed)
--------------------------------------------------------------------------------------- */

/* Generates a short square-wave tone as a raylib Wave in memory. */
static Wave GenerateToneWave(float frequency, float duration, float volume)
{
    int sampleRate = 44100;
    int frameCount = (int)(duration * sampleRate);
    if (frameCount < 1) frameCount = 1;

    short *data = (short *)malloc(frameCount * sizeof(short));

    int fadeSamples = (int)(sampleRate * 0.008f);
    if (fadeSamples < 1) fadeSamples = 1;

    for (int i = 0; i < frameCount; i++)
    {
        float t = (float)i / sampleRate;
        float raw = sinf(2.0f * PI * frequency * t);
        float square = (raw >= 0.0f) ? 1.0f : -1.0f;

        float envelope = 1.0f;
        if (i < fadeSamples) envelope = (float)i / fadeSamples;
        if (i > frameCount - fadeSamples) envelope = (float)(frameCount - i) / fadeSamples;

        data[i] = (short)(square * 3200.0f * volume * envelope);
    }

    Wave wave = { 0 };
    wave.frameCount = (unsigned int)frameCount;
    wave.sampleRate = (unsigned int)sampleRate;
    wave.sampleSize = 16;
    wave.channels = 1;
    wave.data = data;

    return wave;
}

static Sound MakeToneSound(float frequency, float duration, float volume)
{
    Wave w = GenerateToneWave(frequency, duration, volume);
    Sound s = LoadSoundFromWave(w);
    UnloadWave(w); /* LoadSoundFromWave copies the buffer, safe to free our malloc'd wave */
    return s;
}

/* A short rising/falling sweep used for jump & hit feedback. */
static Wave GenerateSweepWave(float startFreq, float endFreq, float duration, float volume)
{
    int sampleRate = 44100;
    int frameCount = (int)(duration * sampleRate);
    if (frameCount < 1) frameCount = 1;
    short *data = (short *)malloc(frameCount * sizeof(short));

    for (int i = 0; i < frameCount; i++)
    {
        float t = (float)i / sampleRate;
        float progress = (float)i / frameCount;
        float freq = startFreq + (endFreq - startFreq) * progress;
        float raw = sinf(2.0f * PI * freq * t);
        float square = (raw >= 0.0f) ? 1.0f : -1.0f;
        float envelope = 1.0f - progress; /* fade out */
        data[i] = (short)(square * 3000.0f * volume * envelope);
    }

    Wave wave = { 0 };
    wave.frameCount = (unsigned int)frameCount;
    wave.sampleRate = (unsigned int)sampleRate;
    wave.sampleSize = 16;
    wave.channels = 1;
    wave.data = data;
    return wave;
}

static void InitAudioAssets(void)
{
    /* Sound effects */
    Wave jumpWave = GenerateSweepWave(300, 700, 0.15f, 0.5f);
    sfxJump = LoadSoundFromWave(jumpWave);
    UnloadWave(jumpWave);

    sfxCoin = MakeToneSound(880.0f, 0.08f, 0.4f);
    sfxBill = MakeToneSound(1318.5f, 0.12f, 0.5f);

    Wave hitWave = GenerateSweepWave(220, 80, 0.2f, 0.6f);
    sfxHit = LoadSoundFromWave(hitWave);
    UnloadWave(hitWave);

    Wave overWave = GenerateSweepWave(400, 60, 0.6f, 0.6f);
    sfxGameOver = LoadSoundFromWave(overWave);
    UnloadWave(overWave);

    /* Background melody: an original upbeat 8-bit tune using a freygish/klezmer-flavored
       scale (D, Eb, F#, G, A, Bb, C, D) -- evokes the mood without copying any
       specific copyrighted composition or arrangement. */
    float scale[8] = {
        146.83f, /* D3  */
        155.56f, /* Eb3 */
        184.99f, /* F#3 */
        196.00f, /* G3  */
        220.00f, /* A3  */
        233.08f, /* Bb3 */
        261.63f, /* C4  */
        293.66f  /* D4  */
    };

    /* Simple up-down arpeggio pattern across 16 steps */
    int pattern[MELODY_LEN] = { 0,2,4,6,7,6,4,2, 0,3,5,7,7,5,3,0 };

    for (int i = 0; i < MELODY_LEN; i++)
    {
        float freq = scale[pattern[i] % 8];
        melodyNotes[i] = MakeToneSound(freq, NOTE_DURATION * 0.9f, 0.28f);
    }
}

static void UnloadAudioAssets(void)
{
    UnloadSound(sfxJump);
    UnloadSound(sfxCoin);
    UnloadSound(sfxBill);
    UnloadSound(sfxHit);
    UnloadSound(sfxGameOver);
    for (int i = 0; i < MELODY_LEN; i++) UnloadSound(melodyNotes[i]);
}

static void UpdateMelody(float dt)
{
    if (screen != SCR_PLAYING) return;
    melodyTimer += dt;
    if (melodyTimer >= NOTE_DURATION)
    {
        melodyTimer = 0.0f;
        PlaySound(melodyNotes[melodyIndex]);
        melodyIndex = (melodyIndex + 1) % MELODY_LEN;
    }
}

/* ---------------------------------------------------------------------------------------
   PARTICLES (small burst effect on collect / hit)
--------------------------------------------------------------------------------------- */

static void SpawnParticles(Vector2 pos, Color color, int count)
{
    int spawned = 0;
    for (int i = 0; i < MAX_PARTICLES && spawned < count; i++)
    {
        if (!particles[i].active)
        {
            particles[i].active = true;
            particles[i].pos = pos;
            float angle = ((float)GetRandomValue(0, 360)) * DEG2RAD;
            float speed = (float)GetRandomValue(60, 180);
            particles[i].vel = (Vector2){ cosf(angle) * speed, sinf(angle) * speed - 80 };
            particles[i].life = 0.5f;
            particles[i].color = color;
            spawned++;
        }
    }
}

static void UpdateParticles(float dt)
{
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        if (particles[i].active)
        {
            particles[i].pos.x += particles[i].vel.x * dt;
            particles[i].pos.y += particles[i].vel.y * dt;
            particles[i].vel.y += 400.0f * dt;
            particles[i].life -= dt;
            if (particles[i].life <= 0) particles[i].active = false;
        }
    }
}

static void DrawParticles(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
        if (particles[i].active)
        {
            Color c = particles[i].color;
            c.a = (unsigned char)(255 * (particles[i].life / 0.5f));
            float size = 2.0f + 3.0f * (particles[i].life / 0.5f);
            DrawCircleV(particles[i].pos, size, c);
        }
    }
}

/* ---------------------------------------------------------------------------------------
   RESET / INIT
--------------------------------------------------------------------------------------- */

static void AddScreenShake(float amount)
{
    if (amount > screenShake) screenShake = amount;
}

static void ResetGame(void)
{
    for (int i = 0; i < MAX_OBSTACLES; i++) obstacles[i].active = false;
    for (int i = 0; i < MAX_COLLECTIBLES; i++) collectibles[i].active = false;
    for (int i = 0; i < MAX_PARTICLES; i++) particles[i].active = false;

    player.x = 140;
    player.y = GROUND_Y;
    player.velY = 0;
    player.isJumping = false;
    player.isDucking = false;
    player.invulnTimer = 0;
    player.rect = (Rectangle){ player.x, player.y - 60, 40, 60 };

    gameTime = 0;
    gameSpeed = 320.0f;
    spawnTimer = 0.6f;
    collectibleSpawnTimer = 0.4f;
    bgScroll = 0;
    cityScroll = 0;
    screenShake = 0;
    hitFlash = 0;
    uiPulse = 0;

    score = 0;
    coinsCollected = 0;
    billsCollected = 0;
    health = 100.0f;

    melodyIndex = 0;
    melodyTimer = 0.0f;
}

/* ---------------------------------------------------------------------------------------
   OUTFIT DRAWING (simple primitive shapes -- cosmetic only, no gameplay effect)
--------------------------------------------------------------------------------------- */

static void DrawCharacter(OutfitType outfit, float x, float y, float w, float h, bool ducking, bool blink)
{
    if (blink) return; /* invulnerability flicker */

    float headR = w * 0.35f;
    float bodyH = ducking ? h * 0.55f : h * 0.75f;
    float bodyTop = y + (h - bodyH);
    Rectangle body = { x - w * 0.4f, bodyTop, w * 0.8f, bodyH };
    Vector2 headPos = { x, bodyTop - headR * 0.9f };

    Color skin = (Color){ 235, 200, 170, 255 };

    switch (outfit)
    {
        case OUTFIT_STREETWEAR:
        {
            DrawRectangleRounded(body, 0.3f, 6, (Color){ 40, 90, 200, 255 }); /* hoodie */
            DrawCircleV(headPos, headR, skin);
            DrawRectangle(headPos.x - headR, headPos.y - headR * 0.9f, headR * 2, headR * 0.6f, (Color){ 230, 120, 30, 255 }); /* cap */
        } break;

        case OUTFIT_MTA:
        {
            DrawRectangleRounded(body, 0.2f, 6, (Color){ 255, 165, 0, 255 }); /* hi-vis vest */
            DrawRectangle(body.x, body.y + body.height * 0.25f, body.width, 6, (Color){ 255, 255, 120, 255 }); /* reflective stripe */
            DrawCircleV(headPos, headR, skin);
            DrawRectangle(headPos.x - headR * 1.05f, headPos.y - headR * 1.1f, headR * 2.1f, headR * 0.7f, (Color){ 245, 200, 30, 255 }); /* hard hat */
        } break;

        case OUTFIT_TOURIST:
        {
            DrawRectangleRounded(body, 0.3f, 6, (Color){ 230, 60, 90, 255 }); /* hawaiian-ish shirt */
            DrawRectangle(body.x, body.y + body.height * 0.3f, body.width, body.height * 0.15f, (Color){ 250, 220, 60, 255 });
            DrawCircleV(headPos, headR, skin);
            DrawRectangle(headPos.x - headR, headPos.y - headR * 0.8f, headR * 2, headR * 0.5f, (Color){ 255, 255, 255, 255 }); /* sun cap */
            DrawRectangle(body.x + body.width - 4, body.y + body.height * 0.2f, 10, 8, (Color){ 20,20,20,255 }); /* camera strap detail */
        } break;

        case OUTFIT_HASIDIC:
        {
            /* Simple, respectful, non-caricatured depiction: dark coat, dark hat.
               Purely a cosmetic skin choice like the others above. */
            DrawRectangleRounded(body, 0.15f, 6, (Color){ 25, 25, 30, 255 }); /* long dark coat */
            DrawRectangle(body.x, body.y, body.width, body.height, (Color){ 25,25,30,255 });
            DrawCircleV(headPos, headR, skin);
            DrawRectangle(headPos.x - headR * 1.1f, headPos.y - headR * 0.6f, headR * 2.2f, headR * 0.35f, (Color){ 15,15,15,255 }); /* hat brim */
            DrawRectangle(headPos.x - headR * 0.6f, headPos.y - headR * 1.2f, headR * 1.2f, headR * 0.7f, (Color){ 15,15,15,255 }); /* hat top */
        } break;

        default: break;
    }
}

/* ---------------------------------------------------------------------------------------
   PLAYER
--------------------------------------------------------------------------------------- */

static void UpdatePlayer(float dt)
{
    const float gravity = 1600.0f;
    const float jumpVel = -620.0f;

    if ((IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) && !player.isJumping && !player.isDucking)
    {
        player.velY = jumpVel;
        player.isJumping = true;
        PlaySound(sfxJump);
    }

    player.isDucking = (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S)) && !player.isJumping;

    if (player.isJumping)
    {
        player.velY += gravity * dt;
        player.y += player.velY * dt;
        if (player.y >= GROUND_Y)
        {
            player.y = GROUND_Y;
            player.velY = 0;
            player.isJumping = false;
        }
    }
    else
    {
        player.y = GROUND_Y;
    }

    float h = player.isDucking ? 34.0f : 60.0f;
    float w = 36.0f;
    player.rect = (Rectangle){ player.x - w/2, player.y - h, w, h };

    if (player.invulnTimer > 0) player.invulnTimer -= dt;
}

/* ---------------------------------------------------------------------------------------
   OBSTACLES
--------------------------------------------------------------------------------------- */

static void SpawnObstacle(void)
{
    for (int i = 0; i < MAX_OBSTACLES; i++)
    {
        if (!obstacles[i].active)
        {
            ObstacleType type = (ObstacleType)GetRandomValue(0, 3);
            SizeTier tier;
            int roll = GetRandomValue(0, 99);
            if (roll < 55) tier = SIZE_SMALL;
            else if (roll < 85) tier = SIZE_MEDIUM;
            else tier = SIZE_LARGE;

            Rectangle r;
            switch (type)
            {
                case OBS_POTHOLE:
                {
                    float wdt = 40 + tier * 20;
                    r = (Rectangle){ SCREEN_W + 20, GROUND_Y - 10, wdt, 14 };
                } break;
                case OBS_VENT:
                {
                    float ht = 40 + tier * 22;
                    r = (Rectangle){ SCREEN_W + 20, GROUND_Y - ht, 34, ht };
                } break;
                case OBS_DEBRIS:
                {
                    float wdt = 34 + tier * 14;
                    r = (Rectangle){ SCREEN_W + 20, 40, wdt, 34 + tier * 8 };
                } break;
                case OBS_BEAM:
                default:
                {
                    float ht = 30 + tier * 10;
                    r = (Rectangle){ SCREEN_W + 20, 0, 100 + tier * 20, ht + 90 };
                } break;
            }

            obstacles[i].rect = r;
            obstacles[i].type = type;
            obstacles[i].tier = tier;
            obstacles[i].active = true;
            obstacles[i].scored = false;
            break;
        }
    }
}

static int DamageForTier(SizeTier tier)
{
    switch (tier)
    {
        case SIZE_SMALL: return 8;
        case SIZE_MEDIUM: return 15;
        case SIZE_LARGE: default: return 24;
    }
}

static void UpdateObstacles(float dt)
{
    spawnTimer -= dt;
    if (spawnTimer <= 0)
    {
        SpawnObstacle();
        float interval = 1.5f - (gameSpeed - 320.0f) * 0.0009f;
        if (interval < 0.55f) interval = 0.55f;
        spawnTimer = interval + ((float)GetRandomValue(-10, 10)) / 100.0f;
    }

    for (int i = 0; i < MAX_OBSTACLES; i++)
    {
        if (obstacles[i].active)
        {
            obstacles[i].rect.x -= gameSpeed * dt;
            if (obstacles[i].rect.x + obstacles[i].rect.width < 0) obstacles[i].active = false;

            if (player.invulnTimer <= 0 && CheckCollisionRecs(player.rect, obstacles[i].rect))
            {
                int dmg = DamageForTier(obstacles[i].tier);
                health -= dmg;
                player.invulnTimer = 1.0f;
                hitFlash = 0.18f;
                AddScreenShake(10.0f);
                PlaySound(sfxHit);
                SpawnParticles((Vector2){ player.rect.x + player.rect.width/2, player.rect.y + player.rect.height/2 },
                               (Color){ 200, 40, 40, 255 }, 14);
                if (health <= 0)
                {
                    health = 0;
                    screen = SCR_GAMEOVER;
                    PlaySound(sfxGameOver);
                    if (score > bestScore) bestScore = score;
                }
            }
        }
    }
}

static void DrawObstacle(Obstacle *o)
{
    switch (o->type)
    {
        case OBS_POTHOLE:
            DrawRectangle((int)o->rect.x, (int)o->rect.y, (int)o->rect.width, (int)o->rect.height, (Color){ 20,20,20,255 });
            DrawRectangleLinesEx(o->rect, 2, (Color){ 60,60,60,255 });
            break;
        case OBS_VENT:
            DrawRectangle((int)o->rect.x, (int)o->rect.y, (int)o->rect.width, (int)o->rect.height, (Color){ 90,90,95,255 });
            for (int k = 0; k < 3; k++)
                DrawRectangle((int)o->rect.x + 4, (int)(o->rect.y + 6 + k * 10), (int)o->rect.width - 8, 4, (Color){ 200,200,210,180 });
            break;
        case OBS_DEBRIS:
            DrawRectangle((int)o->rect.x, (int)o->rect.y, (int)o->rect.width, (int)o->rect.height, (Color){ 120, 90, 60, 255 });
            DrawRectangleLinesEx(o->rect, 2, (Color){ 70,50,30,255 });
            break;
        case OBS_BEAM:
            DrawRectangle((int)o->rect.x, (int)o->rect.y, (int)o->rect.width, (int)o->rect.height, (Color){ 70,70,80,255 });
            DrawRectangle((int)o->rect.x, (int)(o->rect.y + o->rect.height - 8), (int)o->rect.width, 8, (Color){ 230,190,20,255 });
            break;
    }
}

/* ---------------------------------------------------------------------------------------
   COLLECTIBLES
--------------------------------------------------------------------------------------- */

static void SpawnCollectible(void)
{
    for (int i = 0; i < MAX_COLLECTIBLES; i++)
    {
        if (!collectibles[i].active)
        {
            bool isBill = GetRandomValue(0, 99) < 18; /* bills rarer than coins */
            float yPos = (float)GetRandomValue(GROUND_Y - 220, GROUND_Y - 30);

            collectibles[i].rect = (Rectangle){ SCREEN_W + 20, yPos, isBill ? 30 : 20, isBill ? 18 : 20 };
            collectibles[i].type = isBill ? COL_BILL : COL_COIN;
            collectibles[i].value = isBill ? 25 : 5;
            collectibles[i].active = true;
            break;
        }
    }
}

static void UpdateCollectibles(float dt)
{
    collectibleSpawnTimer -= dt;
    if (collectibleSpawnTimer <= 0)
    {
        SpawnCollectible();
        collectibleSpawnTimer = 0.5f + ((float)GetRandomValue(0, 60)) / 100.0f;
    }

    for (int i = 0; i < MAX_COLLECTIBLES; i++)
    {
        if (collectibles[i].active)
        {
            collectibles[i].rect.x -= gameSpeed * dt;
            if (collectibles[i].rect.x + collectibles[i].rect.width < 0) collectibles[i].active = false;

            if (CheckCollisionRecs(player.rect, collectibles[i].rect))
            {
                score += collectibles[i].value;
                Vector2 center = { collectibles[i].rect.x + collectibles[i].rect.width/2,
                                    collectibles[i].rect.y + collectibles[i].rect.height/2 };
                if (collectibles[i].type == COL_BILL)
                {
                    billsCollected++;
                    uiPulse = 0.18f;
                    AddScreenShake(2.0f);
                    PlaySound(sfxBill);
                    SpawnParticles(center, (Color){ 40, 170, 90, 255 }, 10);
                }
                else
                {
                    coinsCollected++;
                    uiPulse = 0.12f;
                    PlaySound(sfxCoin);
                    SpawnParticles(center, (Color){ 250, 210, 40, 255 }, 6);
                }
                collectibles[i].active = false;
            }
        }
    }
}

static void DrawCollectible(Collectible *c)
{
    if (c->type == COL_COIN)
    {
        Vector2 center = { c->rect.x + c->rect.width/2, c->rect.y + c->rect.height/2 };
        DrawCircleV(center, c->rect.width/2, (Color){ 250, 210, 40, 255 });
        DrawCircleLines((int)center.x, (int)center.y, c->rect.width/2, (Color){ 180,140,10,255 });
    }
    else
    {
        DrawRectangleRounded(c->rect, 0.3f, 4, (Color){ 40, 170, 90, 255 });
        DrawRectangleLinesEx(c->rect, 1.5f, (Color){ 20, 110, 60, 255 });
        DrawText("$", (int)c->rect.x + 9, (int)c->rect.y + 1, 16, (Color){ 230, 255, 230, 255 });
    }
}

/* ---------------------------------------------------------------------------------------
   BACKGROUND -- scrolling subway tunnel
--------------------------------------------------------------------------------------- */

static void DrawTunnelBackground(void)
{
    /*
       Layered background:
       1) dark ceiling/walls
       2) distant NYC silhouettes
       3) tiled tunnel wall
       4) perspective rails / floor
       5) repeating lights
       This keeps the scene readable while making movement feel much faster.
    */
    ClearBackground((Color){ 10, 12, 18, 255 });

    DrawRectangleGradientV(0, 0, SCREEN_W, GROUND_Y + 20,
                           (Color){ 22, 25, 36, 255 },
                           (Color){ 8, 9, 13, 255 });

    /* Distant city/tunnel silhouettes. */
    int skylineOffset = ((int)(cityScroll * 0.18f)) % 180;
    for (int x = -skylineOffset; x < SCREEN_W + 180; x += 180)
    {
        int h = 45 + ((x / 180 + 7) % 4) * 18;
        DrawRectangle(x, 155 - h, 130, h, (Color){ 25, 28, 40, 255 });

        for (int wx = x + 14; wx < x + 118; wx += 24)
            for (int wy = 170 - h; wy < 145; wy += 24)
                if (((wx + wy) / 24) % 3 != 0)
                    DrawRectangle(wx, wy, 7, 9, (Color){ 130, 112, 55, 100 });
    }

    /* Tunnel ceiling. */
    DrawRectangle(0, 0, SCREEN_W, 70, (Color){ 16, 18, 26, 255 });
    for (int x = -((int)bgScroll % 80); x < SCREEN_W; x += 80)
        DrawLine(x, 0, x + 45, 70, (Color){ 42, 45, 58, 180 });

    /* Scrolling subway tile pattern. */
    int tileW = 60;
    int offset = ((int)bgScroll) % tileW;
    for (int x = -offset; x < SCREEN_W; x += tileW)
    {
        DrawRectangleLines(x, 70, tileW, GROUND_Y - 50,
                           (Color){ 52, 55, 67, 130 });
        DrawLine(x + 1, 70, x + tileW - 1, 70,
                 (Color){ 75, 78, 88, 90 });
    }

    /* Tunnel support columns with depth highlights. */
    int beamSpacing = 220;
    int beamOffset = ((int)bgScroll) % beamSpacing;
    for (int x = -beamOffset; x < SCREEN_W; x += beamSpacing)
    {
        DrawRectangle(x, 70, 16, GROUND_Y - 50, (Color){ 35, 38, 48, 255 });
        DrawRectangle(x + 2, 70, 3, GROUND_Y - 50, (Color){ 75, 78, 88, 130 });
        DrawRectangle(x + 12, 70, 2, GROUND_Y - 50, (Color){ 8, 9, 13, 180 });
    }

    /* Neon-ish overhead lamps. */
    int lightOffset = ((int)bgScroll) % 150;
    for (int x = -lightOffset; x < SCREEN_W + 150; x += 150)
    {
        DrawRectangle(x, 76, 58, 5, (Color){ 190, 190, 165, 150 });
        DrawRectangle(x + 8, 81, 42, 18, (Color){ 110, 110, 100, 35 });
    }

    /* Floor and track. */
    DrawRectangle(0, GROUND_Y + 20, SCREEN_W, SCREEN_H - GROUND_Y - 20,
                  (Color){ 15, 16, 21, 255 });
    DrawRectangle(0, GROUND_Y + 20, SCREEN_W, 6,
                  (Color){ 72, 74, 84, 255 });

    /* Perspective floor lines. */
    int floorOffset = ((int)(bgScroll * 1.4f)) % 90;
    for (int x = -floorOffset; x < SCREEN_W + 90; x += 90)
    {
        DrawLine(x, GROUND_Y + 28, x - 42, SCREEN_H,
                 (Color){ 43, 45, 53, 180 });
    }

    for (int y = GROUND_Y + 70; y < SCREEN_H; y += 42)
        DrawLine(0, y, SCREEN_W, y, (Color){ 35, 37, 44, 130 });
}

/* ---------------------------------------------------------------------------------------
   HUD
--------------------------------------------------------------------------------------- */

static void DrawHUD(void)
{
    /* Top-left status card. */
    DrawRectangleRounded((Rectangle){ 16, 14, 250, 76 }, 0.18f, 8,
                         (Color){ 8, 10, 16, 210 });
    DrawRectangleRoundedLinesEx((Rectangle){ 16, 14, 250, 76 }, 0.18f, 8, 1.0f,
                              (Color){ 80, 84, 100, 180 });

    DrawText("HEALTH", 28, 21, 13, (Color){ 190, 195, 205, 255 });

    int barW = 190, barH = 15;
    DrawRectangle(28, 42, barW, barH, (Color){ 38, 40, 48, 255 });

    Color hCol = (health > 50)
        ? (Color){ 60, 210, 100, 255 }
        : (health > 25 ? (Color){ 240, 180, 40, 255 }
                       : (Color){ 230, 55, 55, 255 });

    DrawRectangle(28, 42, (int)(barW * (health / 100.0f)), barH, hCol);
    DrawRectangleLines(28, 42, barW, barH, (Color){ 220, 220, 225, 220 });

    char buf[128];
    snprintf(buf, sizeof(buf), "%.0f%%", health);
    DrawText(buf, 226, 42, 14, WHITE);

    /* Score card. */
    DrawRectangleRounded((Rectangle){ SCREEN_W - 225, 14, 209, 92 }, 0.18f, 8,
                         (Color){ 8, 10, 16, 210 });
    DrawRectangleRoundedLinesEx((Rectangle){ SCREEN_W - 225, 14, 209, 92 }, 0.18f, 8, 1.0f,
                              (Color){ 80, 84, 100, 180 });

    snprintf(buf, sizeof(buf), "%06d", score);
    DrawText(buf, SCREEN_W - 207, 24, 30, WHITE);
    DrawText("SCORE", SCREEN_W - 205, 55, 12, (Color){ 160, 165, 175, 255 });

    snprintf(buf, sizeof(buf), "● %d", coinsCollected);
    DrawText(buf, SCREEN_W - 105, 55, 13, (Color){ 250, 210, 40, 255 });
    snprintf(buf, sizeof(buf), "$ %d", billsCollected);
    DrawText(buf, SCREEN_W - 205, 76, 13, (Color){ 70, 200, 120, 255 });

    snprintf(buf, sizeof(buf), "SPEED %.1fx", gameSpeed / 320.0f);
    DrawText(buf, 28, 68, 12, (Color){ 175, 180, 190, 255 });

    /* Small progress pulse when collecting currency. */
    if (uiPulse > 0)
    {
        float alpha = uiPulse / 0.18f;
        DrawCircleLines(SCREEN_W - 35, 87, 12.0f + (1.0f - alpha) * 10.0f,
                        (Color){ 250, 220, 80, (unsigned char)(255 * alpha) });
    }
}

/* ---------------------------------------------------------------------------------------
   SCREENS
--------------------------------------------------------------------------------------- */

static const char *OutfitName(OutfitType o)
{
    switch (o)
    {
        case OUTFIT_STREETWEAR: return "Streetwear";
        case OUTFIT_MTA: return "MTA Worker";
        case OUTFIT_TOURIST: return "Tourist";
        case OUTFIT_HASIDIC: return "Hasidic Dress";
        default: return "";
    }
}

static void DrawMenu(void)
{
    DrawTunnelBackground();

    const char *title = "NYC TUNNEL RUNNER";
    int tw = MeasureText(title, 48);
    DrawText(title, SCREEN_W/2 - tw/2, 60, 48, (Color){ 250, 210, 40, 255 });

    const char *sub = "Choose your commuter (cosmetic only) with LEFT/RIGHT, press ENTER to start";
    int sw = MeasureText(sub, 18);
    DrawText(sub, SCREEN_W/2 - sw/2, 120, 18, (Color){ 220,220,220,255 });

    /* Draw the currently selected character large & centered */
    DrawCharacter(selectedOutfit, SCREEN_W/2, GROUND_Y - 80, 70, 100, false, false);

    const char *name = OutfitName(selectedOutfit);
    int nw = MeasureText(name, 26);
    DrawText(name, SCREEN_W/2 - nw/2, GROUND_Y + 40, 26, WHITE);

    DrawText("<", SCREEN_W/2 - 160, GROUND_Y - 30, 40, (Color){ 200,200,200,255 });
    DrawText(">", SCREEN_W/2 + 145, GROUND_Y - 30, 40, (Color){ 200,200,200,255 });

    const char *ctrls = "Controls: SPACE/UP = Jump    DOWN = Duck";
    int cw = MeasureText(ctrls, 16);
    DrawText(ctrls, SCREEN_W/2 - cw/2, SCREEN_H - 40, 16, (Color){ 180,180,180,255 });

    if (bestScore > 0)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "Best score: %d", bestScore);
        int bw = MeasureText(buf, 18);
        DrawText(buf, SCREEN_W/2 - bw/2, SCREEN_H - 70, 18, (Color){ 250,210,40,255 });
    }
}

static void UpdateMenu(void)
{
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D))
        selectedOutfit = (OutfitType)((selectedOutfit + 1) % OUTFIT_COUNT);
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A))
        selectedOutfit = (OutfitType)((selectedOutfit - 1 + OUTFIT_COUNT) % OUTFIT_COUNT);

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))
    {
        ResetGame();
        screen = SCR_PLAYING;
    }
}

static void DrawGameOver(void)
{
    DrawTunnelBackground();
    for (int i = 0; i < MAX_OBSTACLES; i++) if (obstacles[i].active) DrawObstacle(&obstacles[i]);
    for (int i = 0; i < MAX_COLLECTIBLES; i++) if (collectibles[i].active) DrawCollectible(&collectibles[i]);
    DrawCharacter(selectedOutfit, player.x, player.y, 40, 60, player.isDucking, false);
    DrawParticles();

    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, (Color){ 0,0,0,140 });

    const char *title = "GAME OVER";
    int tw = MeasureText(title, 50);
    DrawText(title, SCREEN_W/2 - tw/2, 150, 50, (Color){ 220, 60, 60, 255 });

    char buf[128];
    snprintf(buf, sizeof(buf), "Score: %d   (Coins x%d, Bills x%d)", score, coinsCollected, billsCollected);
    int bw = MeasureText(buf, 22);
    DrawText(buf, SCREEN_W/2 - bw/2, 220, 22, WHITE);

    snprintf(buf, sizeof(buf), "Best: %d", bestScore);
    bw = MeasureText(buf, 20);
    DrawText(buf, SCREEN_W/2 - bw/2, 250, 20, (Color){ 250,210,40,255 });

    const char *hint = "Press ENTER to return to menu";
    int hw = MeasureText(hint, 18);
    DrawText(hint, SCREEN_W/2 - hw/2, 300, 18, (Color){ 200,200,200,255 });
}

static void UpdateGameOver(void)
{
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))
    {
        screen = SCR_MENU;
    }
}

/* ---------------------------------------------------------------------------------------
   GAME LOOP HELPERS
--------------------------------------------------------------------------------------- */

static void UpdatePlaying(float dt)
{
    gameTime += dt;
    gameSpeed = 320.0f + gameTime * 9.0f;
    if (gameSpeed > 900.0f) gameSpeed = 900.0f;

    bgScroll += gameSpeed * dt;
    cityScroll += gameSpeed * dt;
    score += (int)(gameSpeed * dt * 0.05f);

    UpdatePlayer(dt);
    UpdateObstacles(dt);
    UpdateCollectibles(dt);
    UpdateParticles(dt);
    UpdateMelody(dt);

    if (screenShake > 0) screenShake -= 30.0f * dt;
    if (screenShake < 0) screenShake = 0;

    if (hitFlash > 0) hitFlash -= dt;
    if (uiPulse > 0) uiPulse -= dt;
}

static void DrawPlaying(void)
{
    DrawTunnelBackground();

    for (int i = 0; i < MAX_OBSTACLES; i++)
        if (obstacles[i].active) DrawObstacle(&obstacles[i]);

    for (int i = 0; i < MAX_COLLECTIBLES; i++)
        if (collectibles[i].active) DrawCollectible(&collectibles[i]);

    bool blink = (player.invulnTimer > 0) &&
                 (((int)(player.invulnTimer * 10)) % 2 == 0);

    DrawCharacter(selectedOutfit, player.x, player.y, 40, 60,
                  player.isDucking, blink);

    DrawParticles();
    DrawHUD();

    /* Damage vignette. */
    if (hitFlash > 0)
    {
        unsigned char a = (unsigned char)(150.0f * (hitFlash / 0.18f));
        DrawRectangle(0, 0, SCREEN_W, SCREEN_H, (Color){ 220, 30, 30, a });
    }
}

/* ---------------------------------------------------------------------------------------
   MAIN
--------------------------------------------------------------------------------------- */

int main(void)
{
    SetRandomSeed((unsigned int)time(NULL));

    InitWindow(SCREEN_W, SCREEN_H, "NYC Tunnel Runner");
    SetTargetFPS(60);

    InitAudioDevice();
    InitAudioAssets();

    ResetGame();

    while (!WindowShouldClose())
    {
        /* Clamp dt so a stalled/minimized window cannot break physics. */
        float dt = GetFrameTime();
        if (dt > 0.033f) dt = 0.033f;

        /* ---------------- UPDATE ---------------- */
        switch (screen)
        {
            case SCR_MENU:
                UpdateMenu();
                break;

            case SCR_PLAYING:
                UpdatePlaying(dt);
                break;

            case SCR_GAMEOVER:
                UpdateGameOver();
                UpdateParticles(dt);
                if (screenShake > 0)
                {
                    screenShake -= 30.0f * dt;
                    if (screenShake < 0) screenShake = 0;
                }
                break;
        }

        /* ---------------- DRAW ---------------- */
        BeginDrawing();

        float shakeX = 0.0f;
        float shakeY = 0.0f;

        if (screenShake > 0)
        {
            shakeX = (float)GetRandomValue((int)-screenShake, (int)screenShake);
            shakeY = (float)GetRandomValue((int)-screenShake, (int)screenShake);
        }

        BeginMode2D((Camera2D){
            .offset = { shakeX, shakeY },
            .target = { 0, 0 },
            .rotation = 0,
            .zoom = 1.0f
        });

        switch (screen)
        {
            case SCR_MENU:
                DrawMenu();
                break;

            case SCR_PLAYING:
                DrawPlaying();
                break;

            case SCR_GAMEOVER:
                DrawGameOver();
                break;
        }

        EndMode2D();

        EndDrawing();
    }

    UnloadAudioAssets();
    CloseAudioDevice();
    CloseWindow();

    return 0;
}
