#include "timerISR.h"
#include "helper.h"
#include "periph.h"
#include "spiAVR.h"
#include "serialATmega.h"
#include "LCD.h"

#include <stdlib.h>
#include <stdint.h>
#include <avr/eeprom.h>

/*        Your Name & E-mail: Johnny Wu, jwu519@ucr.edu

 *        Discussion Section: 21

 *        Assignment: Week 10 Final Submissions

 *        Exercise Description: Simple Flappy Bird with menu, score, moving pipes, collision, and fixed jump

 *        I acknowledge all content contained herein, excluding template or example code, is my own original work.

 *

 *        Demo Link: https://youtu.be/idwT4WzAVQE

 */

typedef struct _task {
  signed char state;            // Task's current state
  unsigned long period;         // Task period
  unsigned long elapsedTime;    // Time elapsed since last task tick
  int (*TickFct)(int);          // Task tick function
} task;

// pipe pair
typedef struct _obstacle {
  signed int xPos; // signed because pipes move off the left side of the screen
  unsigned char yPos; // top of the gap
  unsigned char xOffset; // pipe width
  unsigned char yOffset; // gap height
  unsigned int color;
  bool hasPassed;
} obstacle;

typedef struct _sprite {
  signed int xPos;
  signed int yPos;
  unsigned char size;
  unsigned int color;
  signed char speed;
} sprite;

// global variables
#define NUM_TASKS 3
#define MAX_OBSTACLES 10

const unsigned char screenWidth = 128;
const unsigned char screenHeight = 160;

const unsigned char birdSize = 3;
const unsigned char xStart = 30;
const unsigned char yStart = 80;

const unsigned int backgroundColor = 0x0FFF; // aqua
const unsigned int birdColor = 0xFFE0; // yellow
const unsigned int pipeColor = 0x07E0; // green
const unsigned int gameOverColor = 0xF800; // red

// 100ms period for 20 cycles is 2 seconds of hold time
const unsigned char scoreResetTotalTick = 20;

unsigned int score = 0;
unsigned int highScore = 0;

const uint16_t EEPROM_MAGIC_VALUE = 0x00; // custom magic value
uint16_t EEMEM eepromMagic; // place where the magic number will be stored
uint16_t EEMEM eepromHighScore; // place where the high score will be stored

unsigned char numObstacles = 0;
unsigned char nextPipeSpacing = 65;

unsigned char gameReq = 0;
unsigned char gameAck = 0;
unsigned char gameReset = 0;

unsigned char scoreReq = 0;
unsigned char scoreAck = 0;

// task array
task tasks[NUM_TASKS];

// obstacle list
obstacle obstacles[MAX_OBSTACLES];

// controllable object
sprite flappy;

// task periods
const unsigned long MENU_PERIOD = 100;
const unsigned long GAME_PERIOD = 30;
const unsigned long SCORE_PERIOD = 100;

// GCD
const unsigned long GCD_PERIOD = 10;

// useful functions
void sendCommand(unsigned char command) {
  PORTB = SetBit(PORTB, 0, 0);   // A0/DC low = command
  PORTB = SetBit(PORTB, 2, 0);   // CS low = select LCD
  SPI_SEND(command);
  PORTB = SetBit(PORTB, 2, 1);   // CS high = deselect LCD
}

void sendData(unsigned char data) {
  PORTB = SetBit(PORTB, 0, 1);   // A0/DC high = data
  PORTB = SetBit(PORTB, 2, 0);   // CS low = select LCD
  SPI_SEND(data);
  PORTB = SetBit(PORTB, 2, 1);   // CS high = deselect LCD
}

void HardwareReset(){
  PORTC = SetBit(PORTC, 2, 0); // Reset Pin to 0
  _delay_ms(200);
  PORTC = SetBit(PORTC, 2, 1); // Reset Pin to 1
  _delay_ms(200);
}

void ST7735_init(){ 
  HardwareReset();
  PORTB = SetBit(PORTB, 0, 0); // A0 Pin to 0, we are sending commands to the LCD
  SPI_SEND(0x01); // Send the SWRESET command
  _delay_ms(150);
  SPI_SEND(0x11); // Send the SLPOUT command
  _delay_ms(200);
  SPI_SEND(0x3A); // Send the COLMOD command
  PORTB = SetBit(PORTB, 0, 1); // A0 Pin to 1, we are sending data to the LCD
  SPI_SEND(0x05); // 16 bit color mode
  PORTB = SetBit(PORTB, 0, 0); // A0 Pin to 0, we are sending commands to the LCD
  _delay_ms(10);
  SPI_SEND(0x29); // Send the DISPON command
  _delay_ms(200);
} 

void createDrawWindow(unsigned char x0, unsigned char y0,
                      unsigned char x1, unsigned char y1) {
  // column address set (CA SET) 
  sendCommand(0x2A); // this tells the LCD that we are about to send you the left and right x coordinates of the drawing area
  sendData(0x00);
  sendData(x0);
  sendData(0x00);
  sendData(x1);

  // row address set (RA SET)
  sendCommand(0x2B); // this tells the LCD that we are about to send you the top and bottom y coordinates
  sendData(0x00);
  sendData(y0);
  sendData(0x00);
  sendData(y1);

  // write to memory
  sendCommand(0x2C);
}

// updated version of create rectangle, allows for offscreen creation
void createRectangle(unsigned int color, unsigned char x0, unsigned char y0,
                     unsigned char x1, unsigned char y1) {
  if (x1 < 0 || y1 < 0 || x0 >= screenWidth || y0 >= screenHeight) {
    return;
  }
  if (x0 < 0) { // if object goes off page on the left, set it to zero instead
    x0 = 0;
  }
  if (y0 < 0) { // if object tries to go above the page, set it to zero instead
    y0 = 0;
  }
  if (x1 >= screenWidth) { // if object goes off the page on the right, set it to the right edge
    x1 = screenWidth - 1;
  }
  if (y1 >= screenHeight) { // if object goes below the page, set it to the bottom edge
    y1 = screenHeight - 1;
  }

  if (x1 < x0 || y1 < y0) { // if the dimensions of the rectangle makes no sense
    return;
  }

  unsigned int width = x1 - x0 + 1;
  unsigned int height = y1 - y0 + 1;

  unsigned char highByte = color >> 8;
  unsigned char lowByte  = color & 0xFF;

  createDrawWindow(x0, y0, x1, y1);

  PORTB = SetBit(PORTB, 0, 1);   // data mode
  PORTB = SetBit(PORTB, 2, 0);   // select LCD

  for (unsigned long i = 0; i < (unsigned long)width * height; i++) {
    SPI_SEND(highByte);
    SPI_SEND(lowByte);
  }

  PORTB = SetBit(PORTB, 2, 1);   // deselect LCD
}

void loadHighScoreFromEEPROM() {
  if (eeprom_read_word(&eepromMagic) == EEPROM_MAGIC_VALUE) { // if EEPROM magic number address is the magic value
    highScore = (unsigned int)eeprom_read_word(&eepromHighScore); // we load the high score from the high score address
  } else {
    highScore = 0; // high score is invalid, probably garbage
    eeprom_update_word(&eepromHighScore, (uint16_t)highScore); // update the high score with 0 in it
    eeprom_update_word(&eepromMagic, EEPROM_MAGIC_VALUE); // set the magic number back to the predetermined number
  }
}

void saveHighScoreToEEPROM() {
  eeprom_update_word(&eepromHighScore, (uint16_t)highScore); // update the high score
  eeprom_update_word(&eepromMagic, EEPROM_MAGIC_VALUE); // rewrite magic number to ensure that is actually the high score
}

void resetHighScoreInEEPROM() {
  highScore = 0;
  saveHighScoreToEEPROM();
}

void lcd_print_score() {
  char scoreText[11];
  char highText[11];

  itoa(score, scoreText, 10); // converts the long to text
  itoa(highScore, highText, 10);
  lcd_clear();
  lcd_goto_xy(0, 0);
  lcd_write_str("Score: ");
  lcd_write_str(scoreText);
  lcd_goto_xy(1, 0);
  lcd_write_str("H: ");
  lcd_write_str(highText);
  lcd_write_str(" R: Reset");
}

// draw specifically for bird
void drawBird(unsigned int color) {
  createRectangle(color, flappy.xPos - flappy.size, flappy.yPos - flappy.size, flappy.xPos + flappy.size, flappy.yPos + flappy.size);
}

// draw specifically for pipe
void drawPipe(obstacle *currentPipe, unsigned int color) {
  // Each pipe is two rectangles with a gap between them
  createRectangle(color, currentPipe->xPos, 0, currentPipe->xPos + currentPipe->xOffset - 1, currentPipe->yPos - 1); // top pipe
  createRectangle(color, currentPipe->xPos, currentPipe->yPos + currentPipe->yOffset, currentPipe->xPos + currentPipe->xOffset - 1, screenHeight - 1); // bottom pipe
}

void addPipe(signed int newX) {
  unsigned char gapRange;
  if (numObstacles >= MAX_OBSTACLES) {
    return;
  }
  obstacles[numObstacles].xPos = newX;
  obstacles[numObstacles].xOffset = 12 + (rand() % 7); // width from 12 to 18
  obstacles[numObstacles].yOffset = 25 + (rand() % 15); // gap from 25 to 40
  gapRange = screenHeight - obstacles[numObstacles].yOffset - 36;
  if (gapRange < 1) {
    gapRange = 1;
  }
  obstacles[numObstacles].yPos = 18 + (rand() % gapRange);
  obstacles[numObstacles].color = pipeColor;
  obstacles[numObstacles].hasPassed = false;

  numObstacles++;
}

void removePipe(unsigned char index) {
  // Removes one pipe and shifts the rest of the array left.
  if (index >= numObstacles) {
    return;
  }
  for (unsigned char i = index; i < numObstacles - 1; i++) { // shift every value leftwards
    obstacles[i] = obstacles[i + 1];
  }
  numObstacles--;
}

void displayNewGame() {
  gameReq = 1;
  gameAck = 0;

  score = 0;
  flappy.speed = 0;
  numObstacles = 0;
  nextPipeSpacing = 55 + (rand() % 30);

  flappy.color = birdColor;
  flappy.xPos = xStart;
  flappy.yPos = yStart;
  flappy.size = birdSize;

  createRectangle(backgroundColor, 0, 0, screenWidth - 1, screenHeight - 1); // background

  addPipe(screenWidth + 2);
  drawBird(flappy.color);
  lcd_print_score();
}

void displayEndGame() {
  gameAck = 1;
  gameReq = 0;

  if (score > highScore) {
    highScore = score;
    saveHighScoreToEEPROM();
  }

  drawBird(gameOverColor);

  char scoreText[11];
  char highText[11];

  itoa(score, scoreText, 10);
  itoa(highScore, highText, 10);

  lcd_clear();
  lcd_goto_xy(0, 0);
  lcd_write_str("S:");
  lcd_write_str(scoreText);
  lcd_write_str(" H:");
  lcd_write_str(highText);
  lcd_goto_xy(1, 0);
  lcd_write_str("Again | Menu");
}

void displayMenuScreen() {
  createRectangle(backgroundColor, 0, 0, screenWidth - 1, screenHeight - 1);
  lcd_clear();
  lcd_goto_xy(0, 0);
  lcd_write_str("Flappy Bird");
  lcd_goto_xy(1, 0);
  lcd_write_str("L=Play | R=Score");
}

void displayHighScoreScreen() {
  char highText[11];
  ultoa((unsigned long)highScore, highText, 10);
  lcd_clear();
  lcd_goto_xy(0, 0);
  lcd_write_str("High: ");
  lcd_write_str(highText);
  lcd_goto_xy(1, 0);
  lcd_write_str("L=Reset | R=Back");
}

void displayHighScoreResetHoldScreen() {
  lcd_clear();
  lcd_goto_xy(0, 0);
  lcd_write_str("Hold Left...");
  lcd_goto_xy(1, 0);
  lcd_write_str("2s to reset");
}

void displayHighScoreResetDoneScreen() {
  lcd_clear();
  lcd_goto_xy(0, 0);
  lcd_write_str("Score Reset!");
  lcd_goto_xy(1, 0);
  lcd_write_str("You can let go.");
}

// task enumerations
enum menuState {menuStart, menuWait, menuLeftPress, menuRightPress, gameMaster, scoreMaster, gameOverMaster, gameOverLeftPress, gameOverRightPress} menuStates;
enum gameState {gameStart, gameRun, gameButtonHeld, resetButtonHeld} gameStates;
enum scoreState {scoreStart, scoreShow, scoreBackPress, scoreLeftHold, scoreResetDone} scoreStates;

// task functions
int gameTick(int state) {
  unsigned char i = 0;
  signed int birdLeft;
  signed int birdRight;
  signed int birdTop;
  signed int birdBottom;
  bool hit = false;

  if (!gameReq) { // only run the game when the menu asks for it
    return gameStart;
  }

  // Transitions
  switch (state) {
    case gameStart:
      state = gameRun;
      break;
    case gameRun:
      if (GetBit(PINC, 0)) { // left button pressed
        flappy.speed = -5; // this is the actual jump
        state = gameButtonHeld;
      } else if (GetBit(PINC, 1)) {
        state = resetButtonHeld;
      }
      break;
    case gameButtonHeld:
      if (!GetBit(PINC, 0)) { // left button released
        state = gameRun;
      }
      break;
    case resetButtonHeld:
      if (!GetBit(PINC, 1)) { // right button released
        state = gameStart;
        gameReset = 1;
        gameAck = 1;
        gameReq = 0;
      }
  }

  // Actions
  switch (state) {
    case gameRun:
    case gameButtonHeld:
      drawBird(backgroundColor); // erase the old bird
      flappy.speed = flappy.speed + 1; // gravity
      if (flappy.speed > 5) { // terminal velocity
        flappy.speed = 5;
      }
      flappy.yPos = flappy.yPos + flappy.speed;

      // update relevant parameters about the object
      birdLeft = flappy.xPos - flappy.size;
      birdRight = flappy.xPos + flappy.size;
      birdTop = flappy.yPos - flappy.size;
      birdBottom = flappy.yPos + flappy.size;
      // interact with every pipe in the array
      while (i < numObstacles) {
        drawPipe(&obstacles[i], backgroundColor); // erase old pipe from screen
        // level of difficulty after a certain score
        if (score >= 15) {
          obstacles[i].xPos = obstacles[i].xPos - 5; // move pipe left
        } else if (score >= 10) {
          obstacles[i].xPos = obstacles[i].xPos - 4; 
        } else if (score >= 5) {
          obstacles[i].xPos = obstacles[i].xPos - 3;
        } else {
          obstacles[i].xPos = obstacles[i].xPos - 2;
        }
        // check to see if bird hits the pipe
        if (birdRight >= obstacles[i].xPos && birdLeft <= (obstacles[i].xPos + obstacles[i].xOffset - 1)) {
          if (birdTop < obstacles[i].yPos || birdBottom >= obstacles[i].yPos + obstacles[i].yOffset) {
            hit = true;
          }
        }
        // check if bird hits celing or floor
        if ((birdTop <= 0) || (birdBottom >= screenHeight - 1)) {
          hit = true;
        }
        // check to see if you have passed this pipe, and award a point if so
        if (!obstacles[i].hasPassed && obstacles[i].xPos + obstacles[i].xOffset < birdLeft) {
          obstacles[i].hasPassed = true;
          score++;
          lcd_print_score();
        }
        // offscreen check
        if (obstacles[i].xPos + obstacles[i].xOffset < 0) { // if pipe goes offscreen
          removePipe(i); // do not increase i, because the next pipe moved into this spot
        } else {
          drawPipe(&obstacles[i], obstacles[i].color); // draw new pipe on screen
          i++; // only move to the next pipe if this pipe was not removed
        }
      }
      // add a new pipe if the newest pipe has moved far enough left or the rightmost pipe moved far enough
      if (numObstacles == 0 || obstacles[numObstacles - 1].xPos + obstacles[numObstacles - 1].xOffset < screenWidth - nextPipeSpacing) {
        addPipe(screenWidth + 2);
        nextPipeSpacing = 55 + (rand() % 30);
      }
      drawBird(flappy.color); // draw the new bird after moving all the pipes

      // check if hit
      if (hit) {
        displayEndGame();
        return gameStart;
      }
      break;
  }
  return state;
}

int scoreTick(int state) {
  static unsigned char heldTick = 0;
  if (!scoreReq) { // only run the scoreboard when the menu asks for it
    heldTick = 0;
    return scoreStart;
  }

  // Transitions
  switch (state) {
    case scoreStart:
      state = scoreShow;
      scoreAck = 0;
      heldTick = 0;
      break;
    case scoreShow:
      if (!GetBit(PINC, 0) && GetBit(PINC, 1)) { // right button pressed, go back to menu
        heldTick = 0;
        state = scoreBackPress;
      } else if (GetBit(PINC, 0) && !GetBit(PINC, 1)) { // left button pressed, start reset hold timer
        heldTick = 0;
        displayHighScoreResetHoldScreen();
        state = scoreLeftHold;
      }
      break;
    case scoreBackPress:
      if (!GetBit(PINC, 1)) { // right button released
        heldTick = 0;
        scoreReq = 0;
        scoreAck = 1;
        state = scoreStart;
      }
      break;
    case scoreLeftHold:
      if (!GetBit(PINC, 0) && heldTick < scoreResetTotalTick) { // left button released before 2 seconds
        displayHighScoreScreen();
        state = scoreShow;
      } else if (heldTick >= scoreResetTotalTick) {
        resetHighScoreInEEPROM();
        displayHighScoreResetDoneScreen();
        state = scoreResetDone;
      }
      break;
    case scoreResetDone:
      if (!GetBit(PINC, 0)) { // wait for left button release after reset
        heldTick = 0;
        displayHighScoreScreen();
        state = scoreShow;
      }
      break;
  }
  // Actions:
  switch (state) {
    case scoreStart:
      displayHighScoreScreen();
      break;
    case scoreLeftHold:
      heldTick++;
      break;
  }

  return state;
}

int mainMenuTick(int state) {
  // Transitions
  switch (state) {
    case menuStart:
      gameReq = 0;
      gameAck = 0;
      scoreReq = 0;
      scoreAck = 0;
      displayMenuScreen();
      state = menuWait;
      break;
    case menuWait:
      if (GetBit(PINC, 0) && !GetBit(PINC, 1)) { // left button pressed
        state = menuLeftPress;
      } else if (!GetBit(PINC, 0) && GetBit(PINC, 1)) { // right button pressed
        state = menuRightPress;
      }
      break;
    case menuLeftPress:
      if (!GetBit(PINC, 0)) { // left button released
        displayNewGame();
        state = gameMaster;
      }
      break;
    case menuRightPress:
      if (!GetBit(PINC, 1)) { // right button released
        scoreReq = 1;
        scoreAck = 0;
        state = scoreMaster;
      }
      break;
    case gameMaster:
      if (gameAck && !gameReset) {
        state = gameOverMaster;
        gameAck = 0;
        gameReq = 0;
      } else if (gameAck && gameReset) {
        state = menuWait;
        gameAck = 0;
        gameReq = 0;
        gameReset = 0;
        displayMenuScreen();
      }
      break;
    case scoreMaster:
      if (scoreAck) {
        scoreAck = 0;
        scoreReq = 0;
        displayMenuScreen();
        state = menuWait;
      }
      break;
    case gameOverMaster:
      if (GetBit(PINC, 0) && !GetBit(PINC, 1)) { // left button pressed, play again
        state = gameOverLeftPress;
      } else if (!GetBit(PINC, 0) && GetBit(PINC, 1)) { // right button pressed, go to menu
        state = gameOverRightPress;
      }
      break;
    case gameOverLeftPress:
      if (!GetBit(PINC, 0)) { // left button released
        displayNewGame();
        state = gameMaster;
      }
      break;
    case gameOverRightPress:
      if (!GetBit(PINC, 1)) { // right button released
        gameReq = 0;
        gameAck = 0;
        displayMenuScreen();
        state = menuWait;
      }
      break;
  }

  return state;
}

// TimerISR 
void TimerISR() {
  for (unsigned int i = 0; i < NUM_TASKS; i++) {                  // Iterate through each task in the task array
    if (tasks[i].elapsedTime == tasks[i].period) {                // Check if the task is ready to tick
      tasks[i].state = tasks[i].TickFct(tasks[i].state);          // Tick and set the next state for this task
      tasks[i].elapsedTime = 0;                                   // Reset the elapsed time for the next tick
    }
    tasks[i].elapsedTime += GCD_PERIOD;                           // Increment the elapsed time by GCD_PERIOD
  }
}

// main
int main(void) { 
  // Port Initilizations
  DDRC  = 0b000100; // all inputs, except for ST7735 A0 pin
  PORTC = 0b000011; 

  DDRB  = 0b101111; // this has to be all outputs, except the MISO pin
  PORTB = 0b000000;

  DDRD  = 0xFF; // all outputs (LCD SCREEN)
  PORTD = 0xFF;

  // Initialize all tasks:
  tasks[0].period = MENU_PERIOD;
  tasks[0].state = menuStart;
  tasks[0].elapsedTime = MENU_PERIOD;
  tasks[0].TickFct = &mainMenuTick;

  tasks[1].period = GAME_PERIOD;
  tasks[1].state = gameStart;
  tasks[1].elapsedTime = GAME_PERIOD;
  tasks[1].TickFct = &gameTick;

  tasks[2].period = SCORE_PERIOD;
  tasks[2].state = scoreStart;
  tasks[2].elapsedTime = SCORE_PERIOD;
  tasks[2].TickFct = &scoreTick;

  // Hardware Initilizations
  SPI_INIT();
  ST7735_init();
  ADC_init();
  lcd_init();
  lcd_clear();

  // load the saved high score before the menu is displayed
  loadHighScoreFromEEPROM();

  // Give Controllable Object Dimensions
  flappy.color = birdColor;
  flappy.xPos = xStart;
  flappy.yPos = yStart;
  flappy.size = birdSize;

  // Timer Initilizations
  TimerSet(GCD_PERIOD);
  TimerOn();

  while (1) {}

  return 0;
}