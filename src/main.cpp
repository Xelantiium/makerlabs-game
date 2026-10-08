// POCKET BREAKOUT for ESP32-C6 + MAX7219 8x8 matrix + two buttons.
// Same wiring as the snake game. LEFT button moves the paddle left, RIGHT moves it right.
// Press BOTH buttons together to launch the ball.
#include <Arduino.h>
#include <Preferences.h> // Saves the high score in flash so it survives power-off.

constexpr uint8_t DIN_PIN = 19;
constexpr uint8_t CLK_PIN = 18;
constexpr uint8_t CS_PIN = 20;
constexpr uint8_t LEFT_PIN = 21;
constexpr uint8_t RIGHT_PIN = 22;
constexpr uint32_t DEBOUNCE_MS = 20;
constexpr uint32_t PADDLE_MS = 80;   // Paddle speed while a button is held (smaller = faster).
constexpr uint8_t BRIGHTNESS = 2;    // 0 to 15.
constexpr uint8_t ROTATION = 0;      // Same orientation settings as the snake game.
constexpr bool MIRROR_X = false;

enum Mode : uint8_t { DEMO, INTRO, SERVE, PLAY, LOSTBALL, CLEARED, GAMEOVER };
Mode mode = DEMO;
uint32_t modeSince = 0;

// Four brick rows (screen rows 1 to 4). Bit x = column x. Tough bricks need two hits and blink.
const uint8_t LAYOUT[5][4] = {
    {0xFF, 0xFF, 0x00, 0x00},
    {0xFF, 0xFF, 0xFF, 0x00},
    {0xAA, 0x55, 0xAA, 0x55},
    {0xFF, 0x7E, 0x3C, 0x18},
    {0xFF, 0xFF, 0xFF, 0xFF},
};
const uint8_t TOUGH[5][4] = {
    {0x00, 0x00, 0x00, 0x00},
    {0xFF, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x18, 0x18},
    {0x81, 0x00, 0x00, 0x81},
};
// 3x5 digits for level and score screens. Bit 2 is the left pixel.
const uint8_t FONT[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
};

uint8_t bricks[8] = {}, tough[8] = {};
uint8_t pixels[8] = {};
int px = 2, pw = 3;                 // Paddle left column and width.
int bx = 3, by = 5, vx = 1, vy = -1; // Ball position and velocity.
uint8_t lives = 3, level = 0, broken = 0;
uint16_t score = 0, best = 0;
bool newBest = false;
int lastPaddleDir = 0;
uint32_t lastBall = 0, lastDraw = 0, lastPaddleStep = 0, lastPaddleMove = 0;

const uint8_t buttonPins[2] = {LEFT_PIN, RIGHT_PIN};
bool lastRaw[2] = {HIGH, HIGH}, stableButton[2] = {HIGH, HIGH};
uint32_t changedAt[2] = {0, 0};
bool btnL = false, btnR = false, bothEdge = false, bothPrev = false;
uint8_t pressedMask = 0;

Preferences prefs;

// ---------- MAX7219 ----------
void maxWrite(uint8_t address, uint8_t value) {
    digitalWrite(CS_PIN, LOW);
    shiftOut(DIN_PIN, CLK_PIN, MSBFIRST, address);
    shiftOut(DIN_PIN, CLK_PIN, MSBFIRST, value);
    digitalWrite(CS_PIN, HIGH);
}

void initMatrix() {
    pinMode(DIN_PIN, OUTPUT);
    pinMode(CLK_PIN, OUTPUT);
    pinMode(CS_PIN, OUTPUT);
    digitalWrite(CLK_PIN, LOW);
    digitalWrite(CS_PIN, HIGH);
    maxWrite(0x0F, 0);
    maxWrite(0x0C, 0);
    maxWrite(0x09, 0);
    maxWrite(0x0B, 7);
    maxWrite(0x0A, BRIGHTNESS);
    for (uint8_t row = 1; row <= 8; ++row) maxWrite(row, 0);
    maxWrite(0x0C, 1);
}

void putPixel(int x, int y) {
    if (x < 0 || x > 7 || y < 0 || y > 7) return;
    if (MIRROR_X) x = 7 - x;
    for (uint8_t i = 0; i < ROTATION % 4; ++i) {
        int oldX = x;
        x = 7 - y;
        y = oldX;
    }
    pixels[y] |= uint8_t(1U << (7 - x));
}

void drawDigit(uint8_t d, int x0, int y0) {
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 3; ++c)
            if (FONT[d][r] & (4 >> c)) putPixel(x0 + c, y0 + r);
}

void drawNumber(uint16_t n) {
    if (n > 99) n = 99;
    if (n < 10) drawDigit(n, 3, 2);
    else { drawDigit(n / 10, 0, 2); drawDigit(n % 10, 4, 2); }
}

// ---------- Game helpers ----------
int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void setMode(Mode m, uint32_t now) { mode = m; modeSince = now; }

bool brickAt(int x, int y) { return x >= 0 && x < 8 && y >= 0 && y < 8 && (bricks[y] & (1U << x)); }

int bricksLeft() {
    int n = 0;
    for (int i = 0; i < 8; ++i) n += __builtin_popcount(bricks[i]);
    return n;
}

void loadLevel(uint8_t lv) {
    for (int i = 0; i < 8; ++i) bricks[i] = tough[i] = 0;
    for (int r = 0; r < 4; ++r) { bricks[1 + r] = LAYOUT[lv % 5][r]; tough[1 + r] = TOUGH[lv % 5][r]; }
    broken = 0;
}

void hitBrick(int x, int y) {
    uint8_t m = 1U << x;
    if (tough[y] & m) tough[y] &= ~m; // First hit only cracks a tough brick.
    else bricks[y] &= ~m;             // Second hit (or a normal brick) destroys it.
    score++;
    broken++;
}

uint32_t ballInterval() { // Ball speeds up with each level and each brick.
    int v = 250 - level * 15 - broken * 2;
    return v < 70 ? 70 : v;
}

void resetDemo() {
    level++;
    loadLevel(level);
    bx = 3; by = 5; vx = 1; vy = -1;
}

void startGame(uint32_t now) {
    randomSeed(micros());
    score = 0; lives = 3; level = 0; pw = 3; px = 2; newBest = false;
    loadLevel(0);
    setMode(INTRO, now);
}

void endGame(uint32_t now) {
    newBest = score > best;
    if (newBest) { best = score; prefs.putUShort("best", best); }
    setMode(GAMEOVER, now);
}

void movePaddle(int dir, uint32_t now) {
    px = clampInt(px + dir, 0, 8 - pw);
    lastPaddleDir = dir;
    lastPaddleMove = now;
}

void usePaddleInput(uint32_t now) {
    if (btnL == btnR) return; // Neither or both pressed.
    if (now - lastPaddleStep < PADDLE_MS) return;
    lastPaddleStep = now;
    movePaddle(btnL ? -1 : 1, now);
}

void stepBall(uint32_t now) {
    int nx = bx + vx, ny = by + vy;
    if (nx < 0 || nx > 7) { vx = -vx; nx = bx + vx; } // Side walls.
    if (ny < 0) { vy = 1; ny = by + vy; }             // Ceiling.
    if (ny >= 7) {                                    // Paddle row.
        if (nx >= px && nx < px + pw) {
            int rel = nx - px;                        // Where on the paddle it landed.
            vx = (pw == 3) ? rel - 1 : (rel == 0 ? -1 : 1);
            if (vx == 0 && now - lastPaddleMove < 180) vx = lastPaddleDir; // Moving paddle adds spin.
            vy = -1;
            ny = 6;
        } else if (mode == PLAY) {
            bx = nx; by = 7; lives--;
            setMode(LOSTBALL, now);
            return;
        } else { vy = -1; ny = 6; } // Demo safety net.
    } else if (brickAt(nx, ny)) {
        hitBrick(nx, ny);
        vy = -vy;
        ny = by + vy;
        nx = bx + vx;
        if (nx < 0 || nx > 7) { vx = -vx; nx = bx + vx; }
        if (ny < 0 || ny > 6 || brickAt(nx, ny)) { nx = bx; ny = by; } // Boxed in: bounce on the spot.
    }
    bx = nx; by = ny;
    if (bricksLeft() == 0) {
        if (mode == PLAY) setMode(CLEARED, now);
        else resetDemo();
    }
}

void demoAim() { // Attract-mode autopilot: moves the paddle under the falling ball.
    if (vy > 0 && by == 6) {
        int nx = bx + vx;
        if (nx < 0 || nx > 7) nx = bx - vx;
        px = clampInt(nx - (int)random(pw), 0, 8 - pw);
    }
}

void readButtons(uint32_t now) {
    pressedMask = 0;
    for (uint8_t i = 0; i < 2; ++i) {
        bool raw = digitalRead(buttonPins[i]);
        if (raw != lastRaw[i]) { lastRaw[i] = raw; changedAt[i] = now; }
        if (now - changedAt[i] >= DEBOUNCE_MS && raw != stableButton[i]) {
            stableButton[i] = raw;
            if (raw == LOW) pressedMask |= (1U << i);
        }
    }
    btnL = stableButton[0] == LOW;
    btnR = stableButton[1] == LOW;
    bool both = btnL && btnR;
    bothEdge = both && !bothPrev;
    bothPrev = both;
}

void update(uint32_t now) {
    uint32_t t = now - modeSince;
    switch (mode) {
    case DEMO:
        if (pressedMask) { startGame(now); break; }
        if (now - lastBall >= 170) { lastBall = now; demoAim(); stepBall(now); }
        break;
    case INTRO:
        if (t >= 1000) setMode(SERVE, now);
        break;
    case SERVE:
        usePaddleInput(now);
        bx = px + pw / 2; by = 6;
        if (bothEdge || t > 4000) { // Launch.
            vx = random(2) ? 1 : -1; vy = -1;
            lastBall = now;
            setMode(PLAY, now);
        }
        break;
    case PLAY:
        usePaddleInput(now);
        if (now - lastBall >= ballInterval()) { lastBall = now; stepBall(now); }
        break;
    case LOSTBALL:
        if (t >= 800) { if (lives == 0) endGame(now); else setMode(SERVE, now); }
        break;
    case CLEARED:
        if (t >= 1000) {
            level++;
            pw = level >= 5 ? 2 : 3;
            px = clampInt(px, 0, 8 - pw);
            loadLevel(level);
            setMode(INTRO, now);
        }
        break;
    case GAMEOVER:
        if (t > 1500 && pressedMask) startGame(now);
        break;
    }
}

// ---------- Drawing ----------
void drawField(uint32_t now, bool showBall) {
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            if (bricks[y] & (1U << x)) {
                if ((tough[y] & (1U << x)) && ((now / 250) % 2)) continue; // Tough bricks blink.
                putPixel(x, y);
            }
    for (int i = 0; i < pw; ++i) putPixel(px + i, 7);
    if (showBall) putPixel(bx, by);
}

void drawGame(uint32_t now) {
    uint32_t t = now - modeSince;
    for (int i = 0; i < 8; ++i) pixels[i] = 0;
    switch (mode) {
    case DEMO:
    case PLAY:
        drawField(now, true);
        break;
    case INTRO:
        drawNumber(level + 1);
        break;
    case SERVE:
        drawField(now, (now / 200) % 2 == 0);
        for (int i = 0; i < lives; ++i) putPixel(i, 0); // Lives shown as dots while serving.
        break;
    case LOSTBALL:
        drawField(now, (now / 80) % 2 == 0);
        break;
    case CLEARED: { // A bright sweep rises from the bottom.
        int rows = t / 110;
        for (int y = 7; y >= 8 - rows && y >= 0; --y)
            for (int x = 0; x < 8; ++x) putPixel(x, y);
        break;
    }
    case GAMEOVER:
        if (t < 700) {
            for (int i = 0; i < 8; ++i) { putPixel(i, i); putPixel(7 - i, i); }
        } else if (!newBest || (now / 250) % 2 == 0) { // New record blinks.
            drawNumber(score);
        }
        break;
    }
    for (uint8_t row = 0; row < 8; ++row) maxWrite(row + 1, pixels[row]);
}

void setup() {
    pinMode(LEFT_PIN, INPUT_PULLUP);
    pinMode(RIGHT_PIN, INPUT_PULLUP);
    initMatrix();
    prefs.begin("breakout", false);
    best = prefs.getUShort("best", 0);
    randomSeed(micros());
    loadLevel(0);
    setMode(DEMO, millis());
}

void loop() {
    uint32_t now = millis();
    readButtons(now);
    update(now);
    if (now - lastDraw >= 25) { lastDraw = now; drawGame(now); }
    delay(1);
}
