#include <Arduino.h> // Provides Arduino pin, timing and random functions.

constexpr uint8_t DIN_PIN = 19; // Matrix data signal, through a 5 V level shifter.
constexpr uint8_t CLK_PIN = 18; // Matrix clock signal, through the level shifter.
constexpr uint8_t CS_PIN = 20; // Matrix CS/LOAD signal, through the level shifter.
constexpr uint8_t LEFT_PIN = 21; // Connect the reset button between GPIO21 and GND.
constexpr uint8_t RIGHT_PIN = 22; // Connect the turn button between GPIO22 and GND.
constexpr uint32_t STEP_MS = 500; // Milliseconds per move; larger means slower.
constexpr uint32_t DEBOUNCE_MS = 30; // Ignores brief mechanical button bouncing.
constexpr uint8_t BRIGHTNESS = 2; // LED brightness from 0 to 15.
constexpr uint8_t ROTATION = 0; // Rotate the display by 0, 1, 2 or 3 quarter-turns.
constexpr bool MIRROR_X = false; // Set true if the display is reflected.
constexpr bool WRAP_EDGES = false; // Set true to pass through the screen edges.

enum GameState { WAITING, PLAYING, LOST, WON }; // Names the four game states.
GameState state = WAITING; // Wait for the left button before starting.
int8_t snakeX[64], snakeY[64]; // Stores each segment; index 0 is the head.
uint8_t snakeLength = 3; // Starts with three segments.
uint8_t direction = 1; // Directions: 0 up, 1 right, 2 down, 3 left.
const int8_t dx[4] = {0, 1, 0, -1}; // Horizontal change for each direction.
const int8_t dy[4] = {-1, 0, 1, 0}; // Vertical change for each direction.
int8_t foodX = 0, foodY = 0; // Stores the food position.
bool turnQueued = false; // Allows only one right turn per move.
uint8_t pixels[8] = {}; // Stores eight rows of eight LED bits.
uint32_t lastMove = 0, lastDraw = 0; // Tracks movement and display timing.
const uint8_t buttonPins[2] = {LEFT_PIN, RIGHT_PIN}; // Lists reset, then right.
bool lastRaw[2] = {HIGH, HIGH}; // Stores the previous electrical button readings.
bool stableButton[2] = {HIGH, HIGH}; // Stores the debounced button readings.
uint32_t changedAt[2] = {0, 0}; // Records when each button last changed.

void maxWrite(uint8_t address, uint8_t value) { // Sends one MAX7219 command.
    digitalWrite(CS_PIN, LOW); // Begins the transfer.
    shiftOut(DIN_PIN, CLK_PIN, MSBFIRST, address); // Sends the register address.
    shiftOut(DIN_PIN, CLK_PIN, MSBFIRST, value); // Sends the register's value.
    digitalWrite(CS_PIN, HIGH); // Latches the command into the matrix driver.
}

void initMatrix() { // Configures the matrix driver.
    pinMode(DIN_PIN, OUTPUT); // Makes the data pin an output.
    pinMode(CLK_PIN, OUTPUT); // Makes the clock pin an output.
    pinMode(CS_PIN, OUTPUT); // Makes the select pin an output.
    digitalWrite(CLK_PIN, LOW); // Starts with the clock low.
    digitalWrite(CS_PIN, HIGH); // Ends any unfinished transfer.
    maxWrite(0x0F, 0); // Disables the all-LED test mode.
    maxWrite(0x0C, 0); // Blanks the display during setup.
    maxWrite(0x09, 0); // Disables seven-segment number decoding.
    maxWrite(0x0B, 7); // Enables all eight rows.
    maxWrite(0x0A, BRIGHTNESS); // Sets the LED brightness.
    for (uint8_t row = 1; row <= 8; ++row) maxWrite(row, 0); // Clears each row.
    maxWrite(0x0C, 1); // Switches on normal display operation.
}

void putPixel(int x, int y) { // Lights one point in the display buffer.
    if (MIRROR_X) x = 7 - x; // Optionally reflects the horizontal position.
    for (uint8_t i = 0; i < ROTATION % 4; ++i) { // Applies each quarter-turn.
        int oldX = x; // Saves the original horizontal position.
        x = 7 - y; // Calculates the rotated horizontal position.
        y = oldX; // Calculates the rotated vertical position.
    }
    pixels[y] |= uint8_t(1U << (7 - x)); // Sets this LED's bit in its row.
}

bool occupied(int x, int y, int count) { // Checks part of the snake for a cell.
    for (int i = 0; i < count; ++i) { // Checks each requested segment.
        if (snakeX[i] == x && snakeY[i] == y) return true; // Found a segment.
    }
    return false; // No checked segment occupies this cell.
}

void placeFood() { // Chooses an empty cell for the next food.
    int empty = 64 - snakeLength; // Counts the unoccupied cells.
    if (empty == 0) return; // A full board has no room for food.
    int chosen = random(empty); // Chooses an empty-cell index at random.
    for (int y = 0; y < 8; ++y) { // Checks each row.
        for (int x = 0; x < 8; ++x) { // Checks each column.
            if (occupied(x, y, snakeLength)) continue; // Skips snake cells.
            if (chosen-- == 0) { // Stops at the selected empty cell.
                foodX = x; // Stores the food column.
                foodY = y; // Stores the food row.
                return; // Finishes placing food.
            }
        }
    }
}

void startGame(uint32_t now) { // Starts or resets the game.
    randomSeed(micros()); // Uses the player's timing to vary the food sequence.
    snakeLength = 3; // Restores the starting length.
    for (int i = 0; i < snakeLength; ++i) { // Positions the initial segments.
        snakeX[i] = 3 - i; // Places the head at column 3, with its body behind.
        snakeY[i] = 4; // Places all starting segments on row 4.
    }
    direction = 1; // Starts moving right.
    turnQueued = false; // Cancels any old turn request.
    state = PLAYING; // Activates the game.
    placeFood(); // Places the first food.
    lastMove = now; // Gives the player a full interval before the first move.
}

void readButtons(uint32_t now) { // Debounces and handles both buttons.
    uint8_t pressed = 0; // Clears this loop's new button presses.
    for (uint8_t i = 0; i < 2; ++i) { // Reads each button.
        bool raw = digitalRead(buttonPins[i]); // LOW means the button is pressed.
        if (raw != lastRaw[i]) { // Detects an electrical change.
            lastRaw[i] = raw; // Remembers the new electrical reading.
            changedAt[i] = now; // Restarts this button's debounce timer.
        }
        if (now - changedAt[i] >= DEBOUNCE_MS && raw != stableButton[i]) { // Accepts a stable change.
            stableButton[i] = raw; // Stores the debounced reading.
            if (raw == LOW) pressed |= (1U << i); // Records only new presses.
        }
    }
    if (pressed & 1U) { // Gives the left reset button priority.
        startGame(now); // Immediately starts a fresh game.
        return; // Does not also turn when both buttons are pressed together.
    }
    if ((pressed & 2U) && state == PLAYING && stableButton[0] == HIGH) { // Accepts right presses while playing and reset is released.
        turnQueued = true; // Queues one clockwise turn for the next move.
    }
}

void stepSnake() { // Advances the snake by one cell.
    if (turnQueued) direction = (direction + 1) % 4; // Applies a clockwise turn.
    turnQueued = false; // Clears the turn request after using it.
    int nx = snakeX[0] + dx[direction]; // Calculates the next head column.
    int ny = snakeY[0] + dy[direction]; // Calculates the next head row.
    if (WRAP_EDGES) { // Optionally connects opposite edges.
        nx = (nx + 8) % 8; // Wraps the horizontal position.
        ny = (ny + 8) % 8; // Wraps the vertical position.
    } else if (nx < 0 || nx > 7 || ny < 0 || ny > 7) { // Detects a wall collision.
        state = LOST; // Ends the game.
        return; // Leaves the snake inside the board.
    }
    bool eating = nx == foodX && ny == foodY; // Checks whether the head finds food.
    int count = snakeLength - (eating ? 0 : 1); // Excludes the tail when it will move away.
    if (occupied(nx, ny, count)) { // Detects a collision with the remaining body.
        state = LOST; // Ends the game.
        return; // Stops this move.
    }
    int newLength = snakeLength + (eating ? 1 : 0); // Grows only when eating.
    for (int i = newLength - 1; i > 0; --i) { // Moves body positions from tail to head.
        snakeX[i] = snakeX[i - 1]; // Copies the preceding segment's column.
        snakeY[i] = snakeY[i - 1]; // Copies the preceding segment's row.
    }
    snakeX[0] = nx; // Moves the head to its new column.
    snakeY[0] = ny; // Moves the head to its new row.
    snakeLength = newLength; // Saves the updated length.
    if (snakeLength == 64) state = WON; // Wins when the snake fills the board.
    else if (eating) placeFood(); // Places new food after growing.
}

void drawGame(uint32_t now) { // Builds and sends the current display image.
    for (int i = 0; i < 8; ++i) pixels[i] = 0; // Clears the previous image.
    if (state == WAITING) { // Shows a right-pointing arrow before starting.
        for (int x = 1; x <= 6; ++x) putPixel(x, 3); // Draws the arrow shaft.
        putPixel(4, 1); // Draws the upper arrowhead tip.
        putPixel(5, 2); // Draws the upper arrowhead slope.
        putPixel(5, 4); // Draws the lower arrowhead slope.
        putPixel(4, 5); // Draws the lower arrowhead tip.
    } else if (state == LOST) { // Shows an X after a collision.
        for (int i = 0; i < 8; ++i) { // Draws both diagonals.
            putPixel(i, i); // Draws the first diagonal.
            putPixel(7 - i, i); // Draws the second diagonal.
        }
    } else if (state == WON) { // Shows a tick after filling the board.
        putPixel(1, 4); // Starts the short stroke.
        putPixel(2, 5); // Continues the short stroke.
        putPixel(3, 6); // Draws the bottom of the tick.
        for (int x = 4; x <= 7; ++x) putPixel(x, 9 - x); // Draws the long stroke.
    } else { // Shows the active game.
        for (int i = 0; i < snakeLength; ++i) putPixel(snakeX[i], snakeY[i]); // Draws the snake.
        if ((now / 220) % 2 == 0) putPixel(foodX, foodY); // Makes the food blink.
    }
    for (uint8_t row = 0; row < 8; ++row) maxWrite(row + 1, pixels[row]); // Sends all rows.
}

void setup() { // Runs once after power-up or hardware reset.
    pinMode(LEFT_PIN, INPUT_PULLUP); // Holds the left input HIGH until pressed.
    pinMode(RIGHT_PIN, INPUT_PULLUP); // Holds the right input HIGH until pressed.
    initMatrix(); // Prepares the LED matrix.
    drawGame(millis()); // Displays the starting arrow.
}

void loop() { // Repeats continuously while the board has power.
    uint32_t now = millis(); // Reads elapsed time in milliseconds.
    readButtons(now); // Processes button presses without blocking movement.
    if (state == PLAYING && now - lastMove >= STEP_MS) { // Checks whether a move is due.
        lastMove = now; // Restarts the movement timer.
        stepSnake(); // Moves the snake once.
    }
    if (now - lastDraw >= 40) { // Refreshes the image every 40 milliseconds.
        lastDraw = now; // Restarts the display timer.
        drawGame(now); // Updates the visible LEDs.
    }
    delay(1); // Yields briefly to the ESP32 background tasks.
}