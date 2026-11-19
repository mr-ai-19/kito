/*
  ================================================================
   K I T O   —   Desktop Companion Robot
   FINAL FIRMWARE v2.0
  ================================================================
   MCU     : ESP32-C3 SuperMini
   Display : SH1106 128x64 OLED (I2C)
   Input   : 1x capacitive touch sensor (TTP223)
   Audio   : passive buzzer
   Power   : LiPo + deep sleep

   ---------------- WIRING ----------------
     Touch OUT  -> GPIO2   (HIGH = touched)
     OLED SDA   -> GPIO8
     OLED SCL   -> GPIO9
     Buzzer +   -> GPIO3
   ----------------------------------------

   ---------------- GESTURES --------------
   Gestures are classified only AFTER the touch fully ends, so
   rubbing can never accidentally trigger a menu.

     1 TAP     -> pet  (reaction depends on bond + affection)
     2 TAPS    -> GAMES MENU
     3 TAPS    -> CLOCK
     RUB       -> best reaction (rubbing is rewarded)
     HOLD 4s   -> sleepy warning
     HOLD 7s   -> "release to sleep" -> deep sleep
   ----------------------------------------

   ---------------- PERSONALITY -----------
   BOND      0..10   grows with attention, never falls
   AFFECTION 0..100  decays each session, recovers with love
                     -> drives Kito's resting face

   Kito remembers across hard reboots (NVS flash):
     name, birthday, bond, affection, mood, sessions,
     games played, pets, high scores, settings

   ---------------- LIBRARIES -------------
     Adafruit SH110X
     Adafruit GFX
  ================================================================
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include "bitmaps.h"

// ================== PINS ==================
#ifdef TOUCH_PIN
  #undef TOUCH_PIN
#endif
#define TOUCH_PIN    2
#define TOUCH_GPIO   GPIO_NUM_2
#define KITO_SDA     8
#define KITO_SCL     9
#define BUZZER_PIN   3
#define OLED_ADDR    0x3C

// ================== TIMING ==================
#define SETTLE_MS       420    // gesture settle window
#define TAP_MAX_MS      400
#define WARN_MS         4000
#define SLEEP_MS        7000
#define MENU_SELECT_MS  1200
#define GAME_QUIT_MS    2000
#define MENU_TIMEOUT    15000

// ================== PERSONALITY TUNING ==================
#define AFF_MAX          100
#define AFF_TAP            3    // affection gained per tap
#define AFF_RUB            6    // per rub
#define AFF_GAME           4    // per game played
#define AFF_SESSION_DECAY  8    // lost each time Kito is powered on
#define AFF_IDLE_DECAY     1    // lost per 30 min ignored while awake

#define BORED_MS   (15UL * 60UL * 1000UL)
#define LONELY_MS  (45UL * 60UL * 1000UL)

enum Mood { MOOD_NORMAL, MOOD_BORED, MOOD_LONELY, MOOD_SLEEPY, MOOD_GROGGY, MOOD_PLAYFUL };

// ================== DISPLAY ==================
// ================================================================
//  DEAD-ROW WORKAROUND
//  Top 16 rows of this panel are physically dead. This subclass adds
//  a fixed +16 offset to EVERY pixel, so all screens (faces, games,
//  menus, text) that are designed for a 48px-tall area render into
//  the working rows 16..63. Anything a screen tries to draw below
//  logical row 47 is clipped (it would fall off the bottom).
//
//  Screens are authored for a 48px logical height. VH = 48.
// ================================================================
#define DEAD_ROWS 16
#define VH        48        // logical usable height

class KitoDisplay : public Adafruit_SH1106G {
public:
  KitoDisplay(uint16_t w, uint16_t h, TwoWire *twi, int8_t rst)
    : Adafruit_SH1106G(w, h, twi, rst) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    y += DEAD_ROWS;
    if (y < DEAD_ROWS || y > 63) return;
    Adafruit_SH1106G::drawPixel(x, y, color);
  }
};

KitoDisplay display(128, 64, &Wire, -1);
Preferences prefs;

// ================== SETTINGS (NVS) ==================
bool soundOn     = true;
int  volumeLevel = 2;      // 0=mute 1=soft 2=normal 3=loud
int  brightness  = 255;
bool idleChatty  = true;   // random micro-idle sounds
bool clock24h    = false;  // false = 12h

// ================== PERSONALITY (NVS) ==================
char kitoName[12] = "Kito";
int  bond       = 0;       // 0..10
int  affection  = 60;      // 0..100
int  savedMood  = 0;

// ---- Life stats (NVS) ----
int  sessions   = 0;
int  gamesTotal = 0;
int  petsTotal  = 0;
int  rubsTotal  = 0;
unsigned long awakeMinTotal = 0;
int  bDay = 0, bMon = 0;   // birthday (0 = not set)
int  lastBdayYear = 0;

// ---- High scores ----
int hsDino=0, hsFlappy=0, hsReact=9999, hsTiming=0, rpsWins=0, coinBest=0, pomoTotal=0;

// ================== RUNTIME ==================
bool timeOK = false;
Mood mood = MOOD_NORMAL;
unsigned long lastTouchAt   = 0;
unsigned long nextMoodEvent = 0;
unsigned long nextIdleDecay = 0;
unsigned long sessionStart  = 0;
bool greeted = false;

// ================== FORWARD DECLS ==================
enum Gesture { G_NONE, G_TAP, G_DOUBLE, G_TRIPLE, G_RUB, G_SLEEP };
struct GameInput { bool tap=false; bool quit=false; };

void      centerText(const char* s, int y, int size);
void      showBitmap(const unsigned char* b);
void      showFace(FaceID id);
bool      playAnim(AnimID id, bool abortOnTouch=false);
void      playAnimLoops(AnimID id, int n);
Gesture   readGesture();
GameInput pollGameInput();
void      resetGameInput();
void      tone2(int f, int d);
void      sweep(int a, int b, int ms);
FaceID    restingFace();
void      affAdd(int n);
void      bondAdd(int n);
void      saveLife();
void      moodTick();
void      microIdle();
int       runMenu();
void      drawIcon(int i, int cx, int cy);
void      screenClock();
void      screenStats();
void      screenSettings();
void      screenSetTime();
void      screenSetName();
void      screenSetBirthday();
void      birthdayParty();
bool      isBirthday();
void      gameDino();
void      gameFlappy();
void      gameReaction();
void      gameTiming();
void      gameRPS();
void      gameCoin();
void      gameFocus();
void      fireworks();
void      confetti();
void      sh1106HardWipe();

// ================================================================
//                    S O U N D   E N G I N E
//  Kito "speaks" in pitch sweeps and short chirps, never long beeps.
//  Everything is short, bright, and slightly randomized so it reads
//  as cute rather than mechanical.
// ================================================================

int volDuty() {
  switch (volumeLevel) {
    case 0: return 0;      // mute
    case 1: return 40;     // soft
    case 2: return 120;    // normal
    default: return 200;   // loud
  }
}

// core tone
void tone2(int freq, int dur) {
  if (!soundOn || volumeLevel == 0) { delay(dur); return; }
  ledcWriteTone(BUZZER_PIN, freq);
  ledcWrite(BUZZER_PIN, volDuty());
  delay(dur);
  ledcWriteTone(BUZZER_PIN, 0);
}

void rest(int ms) { delay(ms); }

// pitch glide — the basis of Kito's "voice"
void sweep(int f1, int f2, int ms) {
  if (!soundOn || volumeLevel == 0) { delay(ms); return; }
  int steps = max(6, ms / 6);
  for (int i = 0; i <= steps; i++) {
    int f = f1 + (int)((long)(f2 - f1) * i / steps);
    ledcWriteTone(BUZZER_PIN, f);
    ledcWrite(BUZZER_PIN, volDuty());
    delay(ms / steps);
  }
  ledcWriteTone(BUZZER_PIN, 0);
}

// A little vibrato — makes tones feel "alive" instead of flat
void warble(int base, int ms, int depth = 60) {
  if (!soundOn || volumeLevel == 0) { delay(ms); return; }
  unsigned long t0 = millis();
  while (millis() - t0 < (unsigned long)ms) {
    int f = base + (int)(depth * sin((millis() - t0) / 22.0));
    ledcWriteTone(BUZZER_PIN, f);
    ledcWrite(BUZZER_PIN, volDuty());
    delay(6);
  }
  ledcWriteTone(BUZZER_PIN, 0);
}

// ---------------- Kito's vocabulary ----------------
// Short. Bright. Always ends going up (happy) or down (sad).

void sfxChirp()    { sweep(1900, 2500, 45); }                                   // tiny "!"
void sfxBlip()     { tone2(2200, 25); }                                          // ui tick
void sfxHappy()    { sweep(1500,2100,55); rest(15); sweep(2100,2700,70); }       // :)
void sfxTrill()    { for(int i=0;i<3;i++){ sweep(1800+i*250, 2400+i*250, 45); rest(12);} }
void sfxGiggle()   { for(int i=0;i<4;i++){ tone2(2100+i*130, 38); rest(28);} }
void sfxLove()     { sweep(1400,2200,70); rest(20); warble(2500,180,90); }
void sfxExcited()  { for(int i=0;i<5;i++){ sweep(1500+i*180, 2300+i*180, 42); rest(8);} }
void sfxCurious()  { sweep(900, 1700, 150); }                                    // "hm?"
void sfxHuh()      { sweep(1200,1600,90); rest(25); sweep(1600,1250,110); }      // "huh?"
void sfxSad()      { sweep(1300,850,240); rest(40); sweep(950,600,320); }
void sfxCry()      { for(int i=0;i<3;i++){ sweep(1500-i*180, 900-i*130, 200); rest(60);} }
void sfxGrumble()  { for(int i=0;i<4;i++){ tone2(250+random(0,50), 60); rest(35);} }
void sfxSigh()     { sweep(1150, 640, 380); }
void sfxYawn()     { sweep(600, 1150, 400); rest(40); sweep(1150, 420, 620); }
void sfxSnore()    { sweep(380, 520, 420); rest(120); sweep(500, 360, 500); }
void sfxHiccup()   { tone2(2400, 45); rest(20); tone2(1500, 60); }
void sfxSneeze()   { sweep(1400,1900,140); rest(60); sweep(2600,700,180); }
void sfxConfused() { sweep(1500,1100,110); rest(20); sweep(1100,1550,110); rest(20); sweep(1550,1200,130); }
void sfxAngry()    { for(int i=0;i<3;i++){ tone2(330,70); tone2(240,70);} }
void sfxScared()   { for(int i=0;i<5;i++){ tone2(1900+random(-200,200), 32); rest(18);} }
void sfxProud()    { sweep(1300,1900,70); rest(15); sweep(1900,2500,70); rest(15); tone2(3000,120); }

// ---- UI ----
void sfxTick()     { tone2(2000, 14); }
void sfxSelect()   { sweep(1600,2200,50); rest(15); tone2(2600, 80); }
void sfxBack()     { sweep(2200,1500,80); }
void sfxError()    { tone2(400, 90); rest(30); tone2(300, 140); }

// ---- Game ----
void sfxJump()     { sweep(1700, 2300, 28); }
void sfxFlap()     { sweep(2000, 2500, 22); }
void sfxPoint()    { tone2(2500, 40); }
void sfxCombo(int n) { tone2(2000 + n*120, 45); }
void sfxWin()      { sweep(1500,2000,70); rest(20); sweep(2000,2600,70); rest(20); sweep(2600,3300,140); }
void sfxLose()     { sweep(900, 520, 180); rest(40); sweep(600, 320, 300); }
void sfxCoinFlip() { for(int i=0;i<8;i++){ tone2(2200 + (i%2)*400, 30); rest(20);} }

// ---- Power ----
void sfxBoot()     { sweep(500,1200,180); rest(30); sweep(1200,1900,150); rest(30); sfxHappy(); }
void sfxSleep()    { sfxYawn(); rest(60); sweep(1200,700,280); rest(40); sweep(800,380,420); }

// ---- Celebration ----
void sfxFanfare() {
  int mel[] = {1568, 1568, 1568, 1245, 1865, 1568};
  int dur[] = {110, 110, 110, 160, 160, 300};
  for (int i = 0; i < 6; i++) { tone2(mel[i], dur[i]); rest(25); }
}
void sfxBirthday() {
  // "happy birthday" opening, kept short and cute
  int mel[] = {1047,1047,1175,1047,1397,1319};
  int dur[] = {130,130,220,220,220,420};
  for (int i = 0; i < 6; i++) { tone2(mel[i], dur[i]); rest(30); }
}
void sfxLevelUp()  { for(int i=0;i<4;i++){ sweep(1400+i*300, 1900+i*300, 60); rest(10);} }

// ---- Micro-idle sounds (rare, quiet, cute) ----
void sfxMurmur()   { warble(1500, 160, 70); }
void sfxHum()      { int m[]={1568,1760,1568}; for(int i=0;i<3;i++){ tone2(m[i], 140); rest(40);} }
void sfxPurr()     { for(int i=0;i<3;i++){ warble(420, 200, 40); rest(30);} }

// ================================================================
//                    D I S P L A Y   H E L P E R S
// ================================================================
void showBitmap(const unsigned char* b) {
  display.clearDisplay();
  display.drawBitmap(FACE_X, FACE_Y, b, FACE_W, FACE_H, SH110X_WHITE);
  display.display();
}
void showFace(FaceID id) {
  showBitmap((const unsigned char*)pgm_read_ptr(&FACE_BITMAPS[id]));
}

bool playAnim(AnimID id, bool abortOnTouch) {
  uint8_t n = ANIM_FRAME_COUNTS[id];
  const uint16_t* dl = ANIM_DELAYS[id];
  uint16_t jit = ANIM_JITTER[id];

  for (uint8_t f = 0; f < n; f++) {
    showBitmap((const unsigned char*)pgm_read_ptr(&ANIM_TABLES[id][f]));
    uint16_t d = pgm_read_word(&dl[f]) + (jit ? random(0, jit) : 0);
    unsigned long t0 = millis();
    while (millis() - t0 < d) {
      if (abortOnTouch && digitalRead(TOUCH_PIN) == HIGH) return false;
      delay(4);
    }
  }
  return true;
}
void playAnimLoops(AnimID id, int n) {
  for (int i = 0; i < n; i++) playAnim(id, false);
}

void centerText(const char* s, int y, int size) {
  display.setTextSize(size);
  int16_t x1,y1; uint16_t w,h;
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, y);
  display.print(s);
}

void titleBar(const char* left, const char* right = nullptr) {
  display.fillRect(0, 0, 128, 11, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setTextSize(1);
  display.setCursor(3, 2);
  display.print(left);
  if (right) {
    int16_t x1,y1; uint16_t w,h;
    display.getTextBounds(right, 0, 0, &x1, &y1, &w, &h);
    display.setCursor(125 - w, 2);
    display.print(right);
  }
  display.setTextColor(SH110X_WHITE);
}

// ================================================================
//                      P E R S O N A L I T Y
// ================================================================

// The resting face IS the relationship. This is the heart of it.
FaceID restingFace() {
  if (affection >= 85 && bond >= 7) return FACE_LOVE;
  if (affection >= 70)              return FACE_HAPPY;
  if (affection >= 45)              return FACE_IDLE_FACE;
  if (affection >= 25)              return FACE_MEH;
  if (affection >= 10)              return FACE_DROOPY;
  return FACE_SAD;
}

void showResting() { showFace(restingFace()); }

void affAdd(int n) {
  int old = affection;
  affection = constrain(affection + n, 0, AFF_MAX);
  prefs.putInt("aff", affection);

  // Crossing a threshold upward is a moment — celebrate it
  if (n > 0) {
    if (old < 85 && affection >= 85) { sfxLevelUp(); playAnimLoops(ANIM_LOVEBURST, 1); }
    else if (old < 45 && affection >= 45) sfxHappy();
  }
}

void bondAdd(int n) {
  int old = bond;
  bond = constrain(bond + n, 0, 10);
  if (bond != old) {
    prefs.putInt("bond", bond);
    if (bond > old) {
      sfxLevelUp();
      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      centerText("BOND UP!", 12, 1);
      char b[8]; snprintf(b, sizeof(b), "%d", bond);
      centerText(b, 24, 3);
      display.display();
      delay(1100);
      playAnimLoops(ANIM_SPARKLE, 1);
    }
  }
}

void saveLife() {
  prefs.putInt("aff",    affection);
  prefs.putInt("bond",   bond);
  prefs.putInt("mood",   (int)mood);
  prefs.putInt("games",  gamesTotal);
  prefs.putInt("pets",   petsTotal);
  prefs.putInt("rubs",   rubsTotal);
  prefs.putULong("awake", awakeMinTotal + (millis() - sessionStart) / 60000UL);
}

// ---------------- Reactions to touch ----------------
// Response scales with BOTH bond (do I know you) and affection (am I happy)

void reactTap() {
  lastTouchAt = millis();
  petsTotal++;
  affAdd(AFF_TAP);
  if (petsTotal % 12 == 0) bondAdd(1);

  if (affection < 25) {
    // Neglected — hesitant, unsure
    sfxHuh();
    showFace(FACE_DROOPY);
    delay(700);
    playAnim(ANIM_GLANCE, false);
    showResting();
    return;
  }

  if (bond <= 2) {
    // Doesn't know you yet — shy
    sfxChirp();
    playAnim(ANIM_SHY, false);
    showResting();
  }
  else if (bond <= 6) {
    sfxHappy();
    playAnim(ANIM_BOUNCE, false);
    showFace(FACE_HAPPY);
    delay(900);
    showResting();
  }
  else {
    // Bonded — delighted
    sfxTrill();
    playAnim(ANIM_HEARTBEAT, false);
    showFace(FACE_LOVE);
    delay(1000);
    showResting();
  }
  prefs.putInt("pets", petsTotal);
}

void reactRub() {
  lastTouchAt = millis();
  rubsTotal++;
  affAdd(AFF_RUB);
  if (rubsTotal % 8 == 0) bondAdd(1);

  if (affection < 25) {
    // Was neglected — the rub actually helps, visibly
    sfxCurious();
    showFace(FACE_MEH);
    delay(600);
    sfxHappy();
    playAnim(ANIM_BOUNCE, false);
    showFace(FACE_HAPPY);
    delay(900);
    showResting();
    return;
  }

  if (bond <= 2) {
    sfxGiggle();
    playAnim(ANIM_SHY, false);
    showFace(FACE_HAPPY);
    delay(800);
  }
  else if (bond <= 6) {
    sfxGiggle();
    playAnimLoops(ANIM_LAUGH, 2);
    showFace(FACE_HAPPY);
    delay(900);
  }
  else {
    // The best reaction in the whole firmware
    sfxLove();
    playAnimLoops(ANIM_LAUGH, 2);
    playAnim(ANIM_LOVEBURST, false);
    showFace(FACE_LOVE);
    delay(1200);
  }
  showResting();
  prefs.putInt("rubs", rubsTotal);
}

// ---------------- Mood ----------------
void updateMood() {
  unsigned long idle = millis() - lastTouchAt;
  struct tm ti;
  int hr = -1;
  if (timeOK && getLocalTime(&ti, 50)) hr = ti.tm_hour;

  if (hr >= 0) {
    if (hr >= 23 || hr < 6)            { mood = MOOD_SLEEPY; return; }
    if (hr >= 6 && hr < 9 && !greeted) { mood = MOOD_GROGGY; return; }
  }

  if (affection >= 80 && idle < BORED_MS) { mood = MOOD_PLAYFUL; return; }
  if (idle > LONELY_MS)                     mood = MOOD_LONELY;
  else if (idle > BORED_MS)                 mood = MOOD_BORED;
  else                                      mood = MOOD_NORMAL;
}

// ---------------- Micro-idle: the thing that makes it feel alive ----------------
// Rare, unpredictable, small. Never the same thing twice in a row.
int lastIdleAct = -1;

void microIdle() {
  int r;
  do { r = random(100); } while (r == lastIdleAct);
  lastIdleAct = r;

  switch (mood) {

    // ---------- SLEEPY (night) ----------
    case MOOD_SLEEPY: {
      display.setContrast(30);
      if (r < 30)      { sfxSnore(); playAnim(ANIM_DRIFT, true); }
      else if (r < 55) { sfxYawn();  playAnim(ANIM_YAWN, true); }
      else if (r < 75) { playAnim(ANIM_DBLINK, true); }
      else             { showFace(FACE_DREAMMING); }
      showFace(FACE_DREAMMING);
      nextMoodEvent = millis() + random(9000, 22000);
      break;
    }

    // ---------- GROGGY (morning) ----------
    case MOOD_GROGGY: {
      display.setContrast(brightness);
      if (r < 40)      { sfxYawn();    playAnim(ANIM_YAWN, true); }
      else if (r < 70) { playAnim(ANIM_STRETCH, true); }
      else             { playAnim(ANIM_DBLINK, true); }
      showFace(FACE_SLEEPY);
      nextMoodEvent = millis() + random(7000, 16000);
      break;
    }

    // ---------- LONELY ----------
    case MOOD_LONELY: {
      if (r < 25)      { sfxSad();   playAnim(ANIM_CRY, true); showFace(FACE_SAD); delay(1200); }
      else if (r < 45) { sfxSigh();  playAnim(ANIM_SIGH, true); }
      else if (r < 65) { playAnim(ANIM_LOOK, true); }
      else if (r < 80) { sfxCurious(); playAnim(ANIM_GLANCE, true); }
      else             { playAnim(ANIM_BLINK, true); }
      showResting();
      nextMoodEvent = millis() + random(22000, 50000);
      break;
    }

    // ---------- BORED ----------
    case MOOD_BORED: {
      if (r < 18)      { sfxSigh();    playAnim(ANIM_SIGH, true); }
      else if (r < 32) { playAnim(ANIM_STRETCH, true); }
      else if (r < 44) { sfxGrumble(); playAnim(ANIM_FUME, true); showFace(FACE_BORED); delay(900); }
      else if (r < 56) { playAnim(ANIM_LOOK, true); }
      else if (r < 66) { sfxHiccup();  playAnim(ANIM_HICCUP, true); }
      else if (r < 76) { playAnim(ANIM_THINKING, true); }
      else if (r < 86) { playAnim(ANIM_DBLINK, true); }
      else             { playAnim(ANIM_BLINK, true); }
      showResting();
      nextMoodEvent = millis() + random(14000, 34000);
      break;
    }

    // ---------- PLAYFUL (high affection) ----------
    case MOOD_PLAYFUL: {
      display.setContrast(brightness);
      if (r < 15)      { sfxGiggle();  playAnim(ANIM_GIGGLE, true); }
      else if (r < 28) { sfxTrill();   playAnim(ANIM_WINKING, true); }
      else if (r < 40) { playAnim(ANIM_BOUNCE, true); }
      else if (r < 50) { if (idleChatty) sfxHum(); playAnim(ANIM_SPARKLE, true); }
      else if (r < 60) { playAnim(ANIM_GLANCE, true); }
      else if (r < 68) { sfxPurr();    playAnim(ANIM_HEARTBEAT, true); }
      else if (r < 76) { playAnim(ANIM_STRETCH, true); }
      else if (r < 84) { playAnim(ANIM_DBLINK, true); }
      else             { playAnim(ANIM_BLINK, true); }
      showResting();
      nextMoodEvent = millis() + random(4500, 11000);
      break;
    }

    // ---------- NORMAL ----------
    default: {
      display.setContrast(brightness);
      if (r < 40)      { playAnim(ANIM_BLINK, true); }
      else if (r < 55) { playAnim(ANIM_LOOK, true); }
      else if (r < 64) { playAnim(ANIM_DBLINK, true); }
      else if (r < 71) { if (idleChatty) sfxCurious(); playAnim(ANIM_GLANCE, true); }
      else if (r < 77) { playAnim(ANIM_STRETCH, true); }
      else if (r < 82) { if (idleChatty) sfxMurmur(); playAnim(ANIM_THINKING, true); }
      else if (r < 86) { sfxHiccup();  playAnim(ANIM_HICCUP, true); }
      else if (r < 89) { sfxSneeze();  playAnim(ANIM_SNEEZE, true); }
      else if (r < 92) { playAnim(ANIM_SIGH, true); }
      else if (r < 95) { playAnim(ANIM_GIGGLE, true); }
      else             { playAnim(ANIM_BLINK, true); }
      showResting();
      nextMoodEvent = millis() + random(3500, 9000);
      break;
    }
  }
}

void moodTick() {
  // slow affection decay while ignored
  if (millis() > nextIdleDecay) {
    if (millis() - lastTouchAt > BORED_MS) {
      affection = constrain(affection - AFF_IDLE_DECAY, 0, AFF_MAX);
      prefs.putInt("aff", affection);
    }
    nextIdleDecay = millis() + (30UL * 60UL * 1000UL);
  }

  if (millis() < nextMoodEvent) return;
  updateMood();
  microIdle();
}

// ================================================================
//                    G E S T U R E   E N G I N E
//  Nothing fires until the touch fully ENDS. This makes it
//  structurally impossible for a rub to trigger a menu.
// ================================================================
Gesture readGesture() {
  if (digitalRead(TOUCH_PIN) == LOW) return G_NONE;

  int  contacts   = 0;
  bool warnShown  = false;
  bool sleepArmed = false;
  unsigned long firstDown = millis();

  while (true) {
    // ---- contact ----
    unsigned long downStart = millis();
    while (digitalRead(TOUCH_PIN) == HIGH) {
      unsigned long total = millis() - firstDown;

      // long-hold path only valid on an unbroken first contact
      if (contacts == 0) {
        if (!warnShown && total >= WARN_MS) {
          warnShown = true;
          sfxGrumble();
          showFace(FACE_SLEEPY);
        }
        if (!sleepArmed && total >= SLEEP_MS) {
          sleepArmed = true;
          tone2(600, 120);
          display.clearDisplay();
          display.setTextColor(SH110X_WHITE);
          centerText("RELEASE", 16, 2);
          centerText("to sleep", 40, 1);
          display.display();
        }
      }
      delay(8);
    }
    unsigned long len = millis() - downStart;
    contacts++;

    if (sleepArmed) return G_SLEEP;

    // a long drag with multiple contacts = rubbing
    if (len > TAP_MAX_MS && contacts >= 2) return G_RUB;

    // ---- settle window: did another contact arrive? ----
    unsigned long s0 = millis();
    bool more = false;
    while (millis() - s0 < SETTLE_MS) {
      if (digitalRead(TOUCH_PIN) == HIGH) { more = true; break; }
      delay(6);
    }

    if (!more) {
      // event finished — classify
      if (contacts >= 4) return G_RUB;
      if (contacts == 3) return G_TRIPLE;
      if (contacts == 2) return G_DOUBLE;
      return G_TAP;
    }

    // runaway rub
    if (contacts >= 6) {
      while (digitalRead(TOUCH_PIN) == HIGH) delay(10);
      return G_RUB;
    }
  }
}

// ---------------- In-game input (non-blocking) ----------------
static bool giDown = false;
static unsigned long giAt = 0;

GameInput pollGameInput() {
  GameInput gi;
  bool now = (digitalRead(TOUCH_PIN) == HIGH);

  if (now && !giDown) { giDown = true; giAt = millis(); }
  else if (now && giDown) {
    unsigned long held = millis() - giAt;
    if (held >= GAME_QUIT_MS) {
      gi.quit = true;
      giDown = false;
      while (digitalRead(TOUCH_PIN) == HIGH) delay(10);
      return gi;
    }
    if (held > 450) {   // show quit progress
      int w = map(held, 450, GAME_QUIT_MS, 0, 58);
      display.fillRect(34, 46, 60, 6, SH110X_BLACK);
      display.drawRect(34, 46, 60, 6, SH110X_WHITE);
      display.fillRect(35, 46, w, 4, SH110X_WHITE);
      display.display();
    }
  }
  else if (!now && giDown) {
    unsigned long held = millis() - giAt;
    giDown = false;
    if (held < GAME_QUIT_MS) gi.tap = true;
  }
  return gi;
}

void resetGameInput() {
  giDown = false;
  while (digitalRead(TOUCH_PIN) == HIGH) delay(10);
  delay(120);
}

// ================================================================
//                          C L O C K
//  Triple-tap from idle. Big, clean, with Kito peeking.
// ================================================================
void screenClock() {
  resetGameInput();
  sfxSelect();
  unsigned long t0 = millis();
  int style = 0;

  while (true) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);

    if (!timeOK) {
      centerText("CLOCK", 4, 1);
      centerText("not set", 22, 2);
      centerText("Settings > Set Time", 41, 1);
    } else {
      struct tm ti;
      getLocalTime(&ti, 100);

      int h = ti.tm_hour;
      char ap[3] = "";
      if (!clock24h) {
        strcpy(ap, (h < 12) ? "AM" : "PM");
        h = h % 12;
        if (h == 0) h = 12;
      }

      char t[10];
      snprintf(t, sizeof(t), "%d:%02d", h, ti.tm_min);

      // big time
      display.setTextSize(3);
      int16_t x1,y1; uint16_t w,hh;
      display.getTextBounds(t, 0, 0, &x1, &y1, &w, &hh);
      int tw = w + (clock24h ? 0 : 22);
      int sx = (128 - tw) / 2;
      display.setCursor(sx, 18);
      display.print(t);

      if (!clock24h) {
        display.setTextSize(1);
        display.setCursor(sx + w + 5, 34);
        display.print(ap);
      }

      // blinking colon effect: draw seconds bar instead
      int sw = map(ti.tm_sec, 0, 59, 0, 110);
      display.drawFastHLine(9, 41, 110, SH110X_WHITE);
      display.fillRect(9, 40, sw, 4, SH110X_WHITE);

      // Kito peeks in the corner occasionally
      if ((millis() / 4000) % 3 == 0) {
        display.fillCircle(112, 10, 5, SH110X_WHITE);
        display.fillCircle(122, 10, 5, SH110X_WHITE);
      }
    }

    if ((millis() / 600) % 2 == 0)
      centerText("TAP exit", 45, 1);

    display.display();

    GameInput gi = pollGameInput();
    if (gi.tap || gi.quit) break;
    if (millis() - t0 > 30000) break;
    delay(60);
  }
  sfxBack();
  showResting();
}

// ================================================================
//                    S E T   T I M E   (12h)
// ================================================================
void applyClock(int h24, int m) {
  struct tm t = {0};
  t.tm_year = 125; t.tm_mon = 0; t.tm_mday = 1;
  t.tm_hour = h24; t.tm_min = m; t.tm_sec = 0;
  time_t e = mktime(&t);
  struct timeval tv = { .tv_sec = e, .tv_usec = 0 };
  settimeofday(&tv, NULL);
  timeOK = true;
}

void screenSetTime() {
  int h = 12, m = 0, pm = 0, field = 0;
  bool down = false; unsigned long at = 0, act = millis(), lastTap = 0;
  int rapid = 0;
  resetGameInput();

  while (true) {
    bool now = (digitalRead(TOUCH_PIN) == HIGH);
    int hp = 0;

    if (now && !down) { down = true; at = millis(); }
    else if (now && down) {
      unsigned long held = millis() - at;
      hp = (int)constrain(map(held, 200, 1100, 0, 100), 0L, 100L);
      if (held >= 1100) {
        sfxSelect();
        while (digitalRead(TOUCH_PIN) == HIGH) delay(10);
        delay(120);
        field++; rapid = 0;
        if (field >= 4) {
          int h24 = h % 12; if (pm) h24 += 12;
          applyClock(h24, m);
          sfxHappy();
          display.clearDisplay();
          display.setTextColor(SH110X_WHITE);
          centerText("TIME SET", 14, 1);
          char b[16]; snprintf(b, sizeof(b), "%d:%02d %s", h, m, pm?"PM":"AM");
          centerText(b, 30, 2);
          display.display();
          delay(1400);
          playAnim(ANIM_NOD, false);
          return;
        }
        act = millis(); continue;
      }
    }
    else if (!now && down) {
      unsigned long held = millis() - at;
      down = false;
      if (held < 1100) {
        if (millis() - lastTap < 380) rapid++; else rapid = 0;
        lastTap = millis();
        if (field == 0) { h++; if (h > 12) h = 1; }
        else if (field == 1) {
          int st = (rapid > 6) ? 10 : (rapid > 3) ? 5 : 1;
          m = (m + st) % 60;
        }
        else if (field == 2) pm = !pm;
        else if (field == 3) { field = 0; sfxTick(); act = millis(); continue; }
        sfxTick(); act = millis();
      }
    }

    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    const char* fl = (field==0)?"hour":(field==1)?"min":(field==2)?"am/pm":"ok?";
    titleBar("SET TIME", fl);

    char hh[4], mm[4];
    snprintf(hh, sizeof(hh), "%d", h);
    snprintf(mm, sizeof(mm), "%02d", m);

    int cx = 54;
    display.setTextSize(3);
    int hw = strlen(hh) * 18;
    display.setCursor(cx - hw - 2, 20); display.print(hh);
    display.setCursor(cx, 20);          display.print(":");
    display.setCursor(cx + 16, 20);     display.print(mm);
    display.setTextSize(1);
    display.setCursor(cx + 56, 34);     display.print(pm ? "PM" : "AM");

    bool bl = ((millis()/330) % 2 == 0);
    if (field==0 && bl) display.fillRect(cx-hw-2, 45, hw, 3, SH110X_WHITE);
    if (field==1 && bl) display.fillRect(cx+16, 45, 36, 3, SH110X_WHITE);
    if (field==2 && bl) display.fillRect(cx+56, 44, 14, 2, SH110X_WHITE);
    if (field==3)       display.drawRect(6, 16, 116, 34, SH110X_WHITE);

    if (hp > 0) {
      int w = map(hp, 0, 100, 0, 124);
      display.drawRect(2, 43, 124, 8, SH110X_WHITE);
      display.fillRect(2, 43, w, 8, SH110X_WHITE);
    } else if (bl) {
      centerText(field==3 ? "HOLD save - TAP edit" : "TAP +1 - HOLD next", 44, 1);
    }
    display.display();

    if (millis() - act > 40000) return;
    delay(16);
  }
}

// ================================================================
//                    N A M E   K I T O
//  Tap cycles letters, hold accepts. Naming it makes it yours.
// ================================================================
void screenSetName() {
  // Letter set: A-Z, then a DONE marker as the last "character"
  const char* CH = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  const int NC = 26;

  char buf[12] = "";
  int len = 0;
  int ci  = 0;          // current letter index
  bool onDone = false;  // cursor is parked on the DONE slot
  bool onDel  = false;  // cursor is parked on the DELETE slot

  // The picker cycles:  A..Z  ->  [DEL]  ->  [DONE]  ->  back to A
  // So the user only ever needs TAP (move) and HOLD (choose).
  int slot = 0;                 // 0..25 = letters, 26 = DEL, 27 = DONE
  const int SLOTS = 28;

  bool down = false;
  unsigned long at = 0, act = millis(), lastTap = 0;
  int rapid = 0;

  resetGameInput();

  while (true) {
    bool now = (digitalRead(TOUCH_PIN) == HIGH);
    int hp = 0;

    if (now && !down) { down = true; at = millis(); }

    else if (now && down) {
      unsigned long held = millis() - at;
      hp = (int)constrain(map(held, 200, 900, 0, 100), 0L, 100L);

      if (held >= 900) {
        // ---- HOLD = choose whatever the cursor is on ----
        while (digitalRead(TOUCH_PIN) == HIGH) delay(10);
        delay(120);

        if (slot == 27) {
          // ============ DONE ============
          if (len == 0) { strcpy(buf, "Kito"); len = 4; }
          buf[len] = 0;
          strncpy(kitoName, buf, sizeof(kitoName) - 1);
          kitoName[sizeof(kitoName)-1] = 0;
          prefs.putString("name", kitoName);

          sfxFanfare();
          display.clearDisplay();
          display.setTextColor(SH110X_WHITE);
          centerText("I am", 8, 1);
          centerText(kitoName, 24, 2);
          display.display();
          delay(1500);
          playAnimLoops(ANIM_SPARKLE, 2);
          showFace(FACE_LOVE);
          delay(900);
          return;
        }
        else if (slot == 26) {
          // ============ DELETE ============
          if (len > 0) { len--; buf[len] = 0; sfxBack(); }
          else sfxError();
        }
        else {
          // ============ ADD LETTER ============
          if (len < 9) {
            buf[len++] = CH[slot];
            buf[len] = 0;
            sfxSelect();
          } else sfxError();
        }
        act = millis();
        continue;
      }
    }

    else if (!now && down) {
      unsigned long held = millis() - at;
      down = false;
      if (held < 900) {
        // ---- TAP = move cursor ----
        if (millis() - lastTap < 280) rapid++; else rapid = 0;
        lastTap = millis();
        int step = (rapid > 8) ? 3 : 1;
        slot = (slot + step) % SLOTS;
        sfxTick();
        act = millis();
      }
    }

    // ================= RENDER =================
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);

    char cnt[8];
    snprintf(cnt, sizeof(cnt), "%d/9", len);
    titleBar("NAME ME", cnt);

    // ---- the name so far ----
    display.setTextSize(2);
    char shown[14];
    snprintf(shown, sizeof(shown), "%s", len ? buf : "___");
    int16_t x1, y1; uint16_t w, h;
    display.getTextBounds(shown, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((128 - w) / 2, 15);
    display.print(shown);

    // ---- the picker ----
    bool blink = ((millis() / 300) % 2 == 0);

    if (slot == 27) {
      // DONE
      display.setTextSize(2);
      display.getTextBounds("DONE", 0, 0, &x1, &y1, &w, &h);
      int px = (128 - w) / 2;
      if (blink) {
        display.fillRoundRect(px - 8, 33, w + 16, 20, 4, SH110X_WHITE);
        display.setTextColor(SH110X_BLACK);
      }
      display.setCursor(px, 36);
      display.print("DONE");
      display.setTextColor(SH110X_WHITE);
    }
    else if (slot == 26) {
      // DELETE
      display.setTextSize(2);
      display.getTextBounds("DEL", 0, 0, &x1, &y1, &w, &h);
      int px = (128 - w) / 2;
      if (blink) {
        display.fillRoundRect(px - 8, 33, w + 16, 20, 4, SH110X_WHITE);
        display.setTextColor(SH110X_BLACK);
      }
      display.setCursor(px, 36);
      display.print("DEL");
      display.setTextColor(SH110X_WHITE);
    }
    else {
      // A LETTER — show neighbours so it feels like a dial
      display.setTextSize(1);
      char pv[2] = { CH[(slot + NC - 1) % NC], 0 };
      char nx[2] = { CH[(slot + 1) % NC], 0 };
      display.setCursor(36, 42); display.print(pv);
      display.setCursor(88, 42); display.print(nx);

      display.setTextSize(3);
      char c[2] = { CH[slot], 0 };
      display.getTextBounds(c, 0, 0, &x1, &y1, &w, &h);
      int px = (128 - w) / 2;
      display.setCursor(px, 34);
      display.print(c);
      if (blink) display.drawFastHLine(px - 4, 58, w + 8, SH110X_WHITE);
    }

    // ---- hold progress ring (shows HOLD is doing something) ----
    if (hp > 0) {
      int bw = map(hp, 0, 100, 0, 124);
      display.drawRect(2, 47, 124, 4, SH110X_WHITE);
      display.fillRect(2, 47, bw, 4, SH110X_WHITE);
    }

    display.display();

    // safety timeout
    if (millis() - act > 90000) {
      if (len == 0) { strcpy(kitoName, "Kito"); prefs.putString("name", kitoName); }
      else { buf[len]=0; strncpy(kitoName, buf, sizeof(kitoName)-1);
             prefs.putString("name", kitoName); }
      return;
    }
    delay(16);
  }
}

// ================================================================
//                    B I R T H D A Y
// ================================================================
void screenSetBirthday() {
  int d = 1, mo = 1, field = 0;
  if (bDay) { d = bDay; mo = bMon; }
  bool down=false; unsigned long at=0, act=millis(), lastTap=0;
  int rapid=0;
  const char* MN[] = {"Jan","Feb","Mar","Apr","May","Jun",
                      "Jul","Aug","Sep","Oct","Nov","Dec"};
  resetGameInput();

  while (true) {
    bool now = (digitalRead(TOUCH_PIN) == HIGH);
    int hp=0;

    if (now && !down) { down=true; at=millis(); }
    else if (now && down) {
      unsigned long held = millis()-at;
      hp = (int)constrain(map(held,200,1100,0,100),0L,100L);
      if (held >= 1100) {
        sfxSelect();
        while (digitalRead(TOUCH_PIN)==HIGH) delay(10);
        delay(120);
        field++;
        if (field >= 3) {
          bDay = d; bMon = mo;
          prefs.putInt("bday", bDay);
          prefs.putInt("bmon", bMon);
          sfxFanfare();
          display.clearDisplay();
          display.setTextColor(SH110X_WHITE);
          centerText("MY BIRTHDAY", 10, 1);
          char b[16]; snprintf(b, sizeof(b), "%s %d", MN[mo-1], d);
          centerText(b, 26, 2);
          display.display();
          delay(1600);
          playAnim(ANIM_CELEBRATE, false);
          return;
        }
        act=millis(); continue;
      }
    }
    else if (!now && down) {
      unsigned long held = millis()-at;
      down=false;
      if (held < 1100) {
        if (millis()-lastTap < 320) rapid++; else rapid=0;
        lastTap=millis();
        if (field==0) { int st=(rapid>6)?5:1; d = ((d-1+st)%31)+1; }
        else if (field==1) mo = (mo % 12) + 1;
        else if (field==2) { field=0; sfxTick(); act=millis(); continue; }
        sfxTick(); act=millis();
      }
    }

    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    titleBar("BIRTHDAY", field==0?"day":field==1?"month":"ok?");

    char ds[4]; snprintf(ds,sizeof(ds),"%d",d);
    display.setTextSize(3);
    display.setCursor(24, 22); display.print(ds);
    display.setTextSize(2);
    display.setCursor(62, 26); display.print(MN[mo-1]);

    bool bl = ((millis()/330)%2==0);
    if (field==0 && bl) display.fillRect(24, 40, 30, 3, SH110X_WHITE);
    if (field==1 && bl) display.fillRect(62, 40, 42, 3, SH110X_WHITE);
    if (field==2)       display.drawRect(14, 18, 100, 34, SH110X_WHITE);

    if (hp>0) {
      int w = map(hp,0,100,0,124);
      display.drawRect(2, 43,124,8,SH110X_WHITE);
      display.fillRect(2, 43,w,8,SH110X_WHITE);
    } else if (bl) {
      centerText(field==2?"HOLD save":"TAP +1 - HOLD next", 44, 1);
    }
    display.display();

    if (millis()-act > 40000) return;
    delay(16);
  }
}

bool isBirthday() {
  if (!bDay || !timeOK) return false;
  struct tm ti;
  if (!getLocalTime(&ti, 100)) return false;
  return (ti.tm_mday == bDay && (ti.tm_mon+1) == bMon);
}

void birthdayParty() {
  sfxBirthday();
  confetti();
  playAnimLoops(ANIM_CELEBRATE, 2);

  for (int i = 0; i < 6; i++) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    if (i % 2 == 0) {
      display.fillRect(0, 14, 128, 34, SH110X_WHITE);
      display.setTextColor(SH110X_BLACK);
    }
    centerText("HAPPY", 18, 2);
    centerText("BIRTHDAY!", 34, 2);
    display.display();
    delay(280);
  }
  fireworks();
  playAnimLoops(ANIM_LOVEBURST, 2);
  showFace(FACE_LOVE);
  delay(1200);
  bondAdd(1);
  affAdd(20);
}

// ================================================================
//                  C E L E B R A T I O N   F X
// ================================================================
void confetti() {
  for (int f = 0; f < 14; f++) {
    display.clearDisplay();
    for (int i = 0; i < 34; i++) {
      int x = random(0,128);
      int y = (random(0,64) + f*4) % 64;
      display.drawPixel(x, y, SH110X_WHITE);
      if (i % 3 == 0) display.drawPixel(x+1, y, SH110X_WHITE);
    }
    display.display();
    if (f % 4 == 0) tone2(2200 + f*80, 25);
    delay(34);
  }
}

void fireworks() {
  for (int s = 0; s < 3; s++) {
    int bx = random(24, 104), by = random(12, 30);

    // launch
    for (int y = 63; y > by; y -= 4) {
      display.clearDisplay();
      display.drawFastVLine(bx, y, 4, SH110X_WHITE);
      if (y < 60) display.drawPixel(bx + random(-1,2), y+6, SH110X_WHITE);
      display.display();
      tone2(900 + (63-y)*22, 5);
      delay(12);
    }
    // burst
    sweep(2600, 1100, 120);
    for (int r = 2; r <= 26; r += 3) {
      display.clearDisplay();
      display.drawCircle(bx, by, r, SH110X_WHITE);
      if (r > 5) display.drawCircle(bx, by, r-3, SH110X_WHITE);
      for (int a = 0; a < 12; a++) {
        float ang = a * (PI/6.0);
        int sx = bx + (int)(cos(ang)*(r+3));
        int sy = by + (int)(sin(ang)*(r+3));
        if (sx>=0 && sx<128 && sy>=0 && sy<64) display.drawPixel(sx, sy, SH110X_WHITE);
      }
      for (int i = 0; i < 12; i++)
        display.drawPixel(random(0,128), random(0,64), SH110X_WHITE);
      display.display();
      delay(26);
    }
  }
  sfxFanfare();
}

// ================================================================
//                        S T A T S
//   Kito shows you your shared history. This is the bond, visible.
// ================================================================
void screenStats() {
  int page = 0;
  const int PAGES = 4;
  resetGameInput();
  sfxSelect();
  unsigned long act = millis();

  while (true) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);

    char pg[8]; snprintf(pg, sizeof(pg), "%d/%d", page+1, PAGES);
    titleBar(kitoName, pg);

    char b[28];

    if (page == 0) {
      // ---- Relationship ----
      centerText("BOND", 14, 1);
      for (int i = 0; i < 10; i++) {
        int hx = 10 + i*12;
        if (i < bond) {
          display.fillCircle(hx-2, 28, 2, SH110X_WHITE);
          display.fillCircle(hx+2, 28, 2, SH110X_WHITE);
          display.fillTriangle(hx-4, 29, hx+4, 29, hx, 34, SH110X_WHITE);
        } else {
          display.drawCircle(hx, 30, 2, SH110X_WHITE);
        }
      }
      centerText("AFFECTION", 40, 1);
      display.drawRect(12, 41, 104, 8, SH110X_WHITE);
      int w = map(affection, 0, 100, 0, 102);
      display.fillRect(13, 42, w, 6, SH110X_WHITE);
    }
    else if (page == 1) {
      // ---- Time together ----
      centerText("TOGETHER", 14, 1);
      snprintf(b, sizeof(b), "%d", sessions);
      display.setTextSize(2);
      display.setCursor(10, 26); display.print(b);
      display.setTextSize(1);
      display.setCursor(10, 44); display.print("sessions");

      unsigned long mins = awakeMinTotal + (millis()-sessionStart)/60000UL;
      if (mins >= 60) snprintf(b, sizeof(b), "%luh", mins/60);
      else            snprintf(b, sizeof(b), "%lum", mins);
      display.setTextSize(2);
      display.setCursor(78, 26); display.print(b);
      display.setTextSize(1);
      display.setCursor(78, 44); display.print("awake");
    }
    else if (page == 2) {
      // ---- Affection given ----
      centerText("YOU GAVE ME", 14, 1);
      display.setTextSize(2);
      snprintf(b, sizeof(b), "%d", petsTotal);
      display.setCursor(12, 26); display.print(b);
      display.setTextSize(1);
      display.setCursor(12, 44); display.print("pets");

      display.setTextSize(2);
      snprintf(b, sizeof(b), "%d", rubsTotal);
      display.setCursor(78, 26); display.print(b);
      display.setTextSize(1);
      display.setCursor(78, 44); display.print("cuddles");
    }
    else {
      // ---- Games ----
      centerText("WE PLAYED", 13, 1);
      display.setTextSize(1);
      snprintf(b, sizeof(b), "Games   %d", gamesTotal);
      display.setCursor(6, 24); display.print(b);
      snprintf(b, sizeof(b), "Dino    %d", hsDino);
      display.setCursor(6, 34); display.print(b);
      snprintf(b, sizeof(b), "Flappy  %d", hsFlappy);
      display.setCursor(6, 44); display.print(b);

      if (hsReact < 9999) snprintf(b, sizeof(b), "React %dms", hsReact);
      else                snprintf(b, sizeof(b), "React  --");
      display.setCursor(70, 24); display.print(b);
      snprintf(b, sizeof(b), "Focus %d", pomoTotal);
      display.setCursor(70, 34); display.print(b);
      snprintf(b, sizeof(b), "RPS   %d", rpsWins);
      display.setCursor(70, 44); display.print(b);
    }

    if ((millis()/600)%2==0)
      centerText("TAP next - HOLD exit", 45, 1);
    display.display();

    GameInput gi = pollGameInput();
    if (gi.tap)  { page = (page+1) % PAGES; sfxTick(); act = millis(); }
    if (gi.quit) break;
    if (millis()-act > 30000) break;
    delay(30);
  }
  sfxBack();
  showResting();
}

// ================================================================
//                      S E T T I N G S
//  Everything customizable.
// ================================================================
void screenSettings() {
  const char* IT[] = { "Set Time","Name Me","Birthday","Sound","Volume",
                       "Bright","Clock","Chatty","Reset","Back" };
  const int N = 10;
  int idx = 0;
  bool down=false; unsigned long at=0, act=millis();
  resetGameInput();
  sfxSelect();

  while (true) {
    bool now = (digitalRead(TOUCH_PIN) == HIGH);
    int hp = 0;

    if (now && !down) { down=true; at=millis(); }
    else if (now && down) {
      unsigned long held = millis()-at;
      hp = (int)constrain(map(held, 250, MENU_SELECT_MS, 0, 100), 0L, 100L);
      if (held >= MENU_SELECT_MS) {
        sfxSelect();
        while (digitalRead(TOUCH_PIN)==HIGH) delay(10);
        delay(120);

        switch (idx) {
          case 0: screenSetTime();     break;
          case 1: screenSetName();     break;
          case 2: screenSetBirthday(); break;

          case 3:  // sound on/off
            soundOn = !soundOn;
            prefs.putBool("snd", soundOn);
            if (soundOn) sfxHappy();
            break;

          case 4:  // volume
            volumeLevel = (volumeLevel + 1) % 4;
            prefs.putInt("vol", volumeLevel);
            if (volumeLevel > 0) sfxChirp();
            break;

          case 5:  // brightness
            brightness -= 55;
            if (brightness < 45) brightness = 255;
            prefs.putInt("brt", brightness);
            display.setContrast(brightness);
            sfxTick();
            break;

          case 6:  // 12h / 24h
            clock24h = !clock24h;
            prefs.putBool("c24", clock24h);
            sfxTick();
            break;

          case 7:  // chatty idle sounds
            idleChatty = !idleChatty;
            prefs.putBool("chat", idleChatty);
            if (idleChatty) sfxMurmur();
            break;

          case 8: {  // reset — needs confirmation
            resetGameInput();
            unsigned long t0 = millis();
            bool confirmed = false;
            while (millis()-t0 < 8000) {
              display.clearDisplay();
              display.setTextColor(SH110X_WHITE);
              titleBar("RESET?", "");
              centerText("Forget everything?", 18, 1);
              centerText("bond, name, stats", 30, 1);
              if ((millis()/400)%2==0)
                centerText("HOLD confirm - TAP no", 41, 1);
              display.display();
              GameInput g = pollGameInput();
              if (g.quit) { confirmed = true; break; }
              if (g.tap) break;
              delay(30);
            }
            if (confirmed) {
              prefs.clear();
              sfxCry();
              playAnimLoops(ANIM_CRY, 2);
              showFace(FACE_CRYING);
              delay(1600);
              ESP.restart();
            }
            break;
          }

          case 9: sfxBack(); return;
        }
        resetGameInput();
        act = millis();
        continue;
      }
    }
    else if (!now && down) {
      unsigned long held = millis()-at;
      down = false;
      if (held < MENU_SELECT_MS) {
        idx = (idx+1) % N;
        sfxTick();
        act = millis();
      }
    }

    // ---- render ----
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    char pg[8]; snprintf(pg, sizeof(pg), "%d/%d", idx+1, N);
    titleBar("SETTINGS", pg);

    display.setTextSize(2);
    int16_t x1,y1; uint16_t w,h;
    display.getTextBounds(IT[idx], 0, 0, &x1, &y1, &w, &h);
    display.setCursor((128-w)/2, 20);
    display.print(IT[idx]);

    // value line
    char v[24] = "";
    switch (idx) {
      case 0:
        if (timeOK) {
          struct tm ti; getLocalTime(&ti, 50);
          int hh2 = ti.tm_hour;
          if (!clock24h) { int k = hh2 % 12; if (!k) k = 12;
                           snprintf(v, sizeof(v), "%d:%02d %s", k, ti.tm_min, hh2<12?"AM":"PM"); }
          else snprintf(v, sizeof(v), "%02d:%02d", hh2, ti.tm_min);
        } else snprintf(v, sizeof(v), "not set");
        break;
      case 1: snprintf(v, sizeof(v), "%s", kitoName); break;
      case 2:
        if (bDay) {
          const char* MN[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
          snprintf(v, sizeof(v), "%s %d", MN[bMon-1], bDay);
        } else snprintf(v, sizeof(v), "not set");
        break;
      case 3: snprintf(v, sizeof(v), soundOn ? "ON" : "MUTED"); break;
      case 4: {
        const char* VL[]={"mute","soft","normal","loud"};
        snprintf(v, sizeof(v), "%s", VL[volumeLevel]);
        break;
      }
      case 5: snprintf(v, sizeof(v), "%d%%", map(brightness,45,255,20,100)); break;
      case 6: snprintf(v, sizeof(v), clock24h ? "24 hour" : "12 hour"); break;
      case 7: snprintf(v, sizeof(v), idleChatty ? "ON" : "quiet"); break;
      case 8: snprintf(v, sizeof(v), "erase all"); break;
      case 9: snprintf(v, sizeof(v), ""); break;
    }
    if (v[0]) centerText(v, 42, 1);

    if (hp > 0) {
      int bw = map(hp, 0, 100, 0, 124);
      display.drawRect(2, 43, 124, 8, SH110X_WHITE);
      display.fillRect(2, 43, bw, 8, SH110X_WHITE);
    } else if ((millis()/600)%2==0) {
      centerText("TAP next - HOLD select", 44, 1);
    }
    display.display();

    if (millis()-act > MENU_TIMEOUT) { sfxBack(); return; }
    delay(16);
  }
}

// ================================================================
//                    G A M E   H E L P E R S
// ================================================================
void countdown(const char* hint) {
  for (int i = 3; i > 0; i--) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    char b[4]; snprintf(b, 4, "%d", i);
    centerText(b, 14, 4);
    centerText(hint, 43, 1);
    display.display();
    tone2(1400 + (3-i)*300, 70);
    delay(560);
  }
  sfxChirp();
}

// Kito reacts to YOUR performance — this is what makes games feel shared
void kitoReactScore(int score, int best, bool newBest) {
  gamesTotal++;
  affAdd(AFF_GAME);
  prefs.putInt("games", gamesTotal);
  if (gamesTotal % 10 == 0) bondAdd(1);

  if (newBest) {
    sfxWin();
    confetti();
    playAnimLoops(ANIM_CELEBRATE, 2);
    showFace(FACE_EXCITED);
    delay(900);
  } else if (score >= best * 0.7 && best > 0) {
    sfxProud();
    playAnim(ANIM_PROUD, false);
    showFace(FACE_PROUD);
    delay(800);
  } else {
    sfxLose();
    playAnim(ANIM_SIGH, false);
    showFace(FACE_MEH);
    delay(700);
  }
}

// returns true = retry
bool gameOver(int score, int best, bool newBest) {
  kitoReactScore(score, best, newBest);
  resetGameInput();
  unsigned long t0 = millis();

  while (true) {
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    centerText(newBest ? "NEW BEST!" : "GAME OVER", 3, 1);
    char b[16]; snprintf(b, sizeof(b), "%d", score);
    centerText(b, 15, 3);
    snprintf(b, sizeof(b), "best %d", best);
    centerText(b, 42, 1);
    if ((millis()/500)%2==0) centerText("TAP retry - HOLD menu", 44, 1);
    display.display();

    GameInput gi = pollGameInput();
    if (gi.tap)  return true;
    if (gi.quit) return false;
    if (millis()-t0 > 20000) return false;
    delay(30);
  }
}

// ================================================================
//  GAME 1 : DINO
// ================================================================
void gameDino() {
  while (true) {
    resetGameInput();
    countdown("TAP TO JUMP");

    const int GND = 44, DX = 16, DW = 9, DH = 11;
    float dy = GND-DH, vy = 0;
    bool onGnd = true;
    struct Obs { float x; int w,h; bool on; };
    Obs ob[3]; for (int i=0;i<3;i++) ob[i].on=false;
    float spd = 1.9;
    int score = 0, combo = 0;
    unsigned long lastSp = millis();
    int gap = 1300;
    bool dead = false;
    resetGameInput();

    while (!dead) {
      GameInput gi = pollGameInput();
      if (gi.quit) return;
      if (gi.tap && onGnd) { vy = -6.4; onGnd = false; sfxJump(); }

      vy += 0.55; dy += vy;
      if (dy >= GND-DH) { dy = GND-DH; vy = 0; onGnd = true; }

      if (millis()-lastSp > (unsigned long)gap) {
        for (int i=0;i<3;i++) if (!ob[i].on) {
          ob[i] = { 130, (int)random(5,10), (int)random(9,17), true };
          break;
        }
        lastSp = millis();
        gap = random(900,1600) - (int)(spd*60);
        if (gap < 480) gap = 480;
      }

      for (int i=0;i<3;i++) {
        if (!ob[i].on) continue;
        ob[i].x -= spd;
        if (ob[i].x + ob[i].w < 0) {
          ob[i].on = false; score++; combo++;
          sfxCombo(min(combo,6));
          spd += 0.06; if (spd > 5.2) spd = 5.2;
          continue;
        }
        int ox=(int)ob[i].x, oy=GND-ob[i].h;
        if (DX+DW > ox && DX < ox+ob[i].w && dy+DH > oy) dead = true;
      }

      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      display.drawFastHLine(0, GND, 128, SH110X_WHITE);
      int py=(int)dy;
      display.fillRect(DX, py+4, DW, DH-4, SH110X_WHITE);
      display.fillRect(DX+3, py, DW-3, 6, SH110X_WHITE);
      display.drawPixel(DX+DW-2, py+2, SH110X_BLACK);
      if (onGnd) {
        if ((millis()/100)%2==0) display.drawFastVLine(DX+2, GND, 2, SH110X_WHITE);
        else                     display.drawFastVLine(DX+7, GND, 2, SH110X_WHITE);
      }
      for (int i=0;i<3;i++) {
        if (!ob[i].on) continue;
        int ox=(int)ob[i].x, oy=GND-ob[i].h;
        display.fillRect(ox, oy, ob[i].w, ob[i].h, SH110X_WHITE);
        display.drawFastVLine(ox-1, oy+3, 4, SH110X_WHITE);
        display.drawFastVLine(ox+ob[i].w, oy+5, 4, SH110X_WHITE);
      }
      display.setTextSize(1);
      display.setCursor(2, 1); display.print(score);
      display.setCursor(86, 1); display.print("HI "); display.print(hsDino);
      display.display();
      delay(16);
    }

    sfxLose();
    bool nb = score > hsDino;
    if (nb) { hsDino = score; prefs.putInt("hsDino", hsDino); }
    if (!gameOver(score, hsDino, nb)) return;
  }
}

// ================================================================
//  GAME 2 : FLAPPY
// ================================================================
void gameFlappy() {
  while (true) {
    resetGameInput();
    countdown("TAP TO FLAP");

    const int BX=24, BR=4, GAP=22;
    float by=24, vy=0, spd=1.5;
    struct Pipe { float x; int gy; bool sc, on; };
    Pipe pp[3]; for (int i=0;i<3;i++) pp[i].on=false;
    int score=0, combo=0;
    unsigned long lastP = millis()-900;
    bool dead=false;
    resetGameInput();

    while (!dead) {
      GameInput gi = pollGameInput();
      if (gi.quit) return;
      if (gi.tap) { vy = -3.0; sfxFlap(); }

      vy += 0.34; by += vy;
      if (by-BR < 0 || by+BR > 47) dead = true;

      if (millis()-lastP > 1500) {
        for (int i=0;i<3;i++) if (!pp[i].on) {
          pp[i] = { 130, (int)random(4, 47-GAP-2), false, true };
          break;
        }
        lastP = millis();
      }

      for (int i=0;i<3;i++) {
        if (!pp[i].on) continue;
        pp[i].x -= spd;
        if (pp[i].x + 8 < 0) { pp[i].on=false; continue; }
        if (!pp[i].sc && pp[i].x + 8 < BX-BR) {
          pp[i].sc = true; score++; combo++;
          sfxCombo(min(combo,6));
          spd += 0.05; if (spd > 3.2) spd = 3.2;
        }
        int px=(int)pp[i].x;
        if (BX+BR > px && BX-BR < px+8) {
          if (by-BR < pp[i].gy || by+BR > pp[i].gy+GAP) dead = true;
        }
      }

      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      for (int i=0;i<3;i++) {
        if (!pp[i].on) continue;
        int px=(int)pp[i].x;
        display.fillRect(px, 0, 8, pp[i].gy, SH110X_WHITE);
        display.fillRect(px, pp[i].gy+GAP, 8, 48-(pp[i].gy+GAP), SH110X_WHITE);
      }
      int y=(int)by;
      display.fillCircle(BX, y, BR, SH110X_WHITE);
      display.drawPixel(BX+2, y-1, SH110X_BLACK);
      if (vy < 0) display.drawFastHLine(BX-5, y-1, 3, SH110X_WHITE);
      else        display.drawFastHLine(BX-5, y+2, 3, SH110X_WHITE);
      display.setTextSize(1);
      display.setCursor(2, 1); display.print(score);
      display.setCursor(86, 1); display.print("HI "); display.print(hsFlappy);
      display.display();
      delay(16);
    }

    sfxLose();
    bool nb = score > hsFlappy;
    if (nb) { hsFlappy = score; prefs.putInt("hsFlappy", hsFlappy); }
    if (!gameOver(score, hsFlappy, nb)) return;
  }
}

// ================================================================
//  GAME 3 : REACTION
// ================================================================
void gameReaction() {
  while (true) {
    resetGameInput();
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    centerText("WAIT for", 6, 1);
    centerText("the FACE", 18, 2);
    centerText("then TAP!", 42, 1);
    display.display();
    delay(1900);

    showFace(FACE_DREAMMING);
    sfxSnore();
    unsigned long wait = random(2200, 6200), t0 = millis();
    bool early = false;

    while (millis()-t0 < wait) {
      if (digitalRead(TOUCH_PIN) == HIGH) {
        unsigned long h = millis();
        while (digitalRead(TOUCH_PIN) == HIGH) {
          if (millis()-h > GAME_QUIT_MS) return;
          delay(10);
        }
        early = true; break;
      }
      delay(8);
    }

    if (early) {
      sfxAngry();
      playAnim(ANIM_FUME, false);
      showFace(FACE_ANGRY);
      display.setTextColor(SH110X_WHITE);
      centerText("TOO EARLY!", 42, 1);
      display.display();
      delay(1500);
      resetGameInput();
      if (!gameOver(0, hsReact<9999?hsReact:0, false)) return;
      continue;
    }

    showFace(FACE_SURPRISED);
    sfxChirp();
    unsigned long go = millis();
    while (digitalRead(TOUCH_PIN) == LOW) {
      if (millis()-go > 5000) break;
      delay(1);
    }
    int rt = (int)(millis()-go);
    unsigned long h = millis();
    while (digitalRead(TOUCH_PIN) == HIGH) {
      if (millis()-h > GAME_QUIT_MS) return;
      delay(10);
    }

    bool nb = rt < hsReact;
    if (nb) { hsReact = rt; prefs.putInt("hsReact", hsReact); }

    if (rt < 250)      { showFace(FACE_SUN_GLASSES); sfxProud(); }
    else if (rt < 400) { showFace(FACE_HAPPY);       sfxHappy(); }
    else if (rt < 700) { showFace(FACE_IDLE_FACE);   sfxBlip();  }
    else               { showFace(FACE_MEH);         sfxSad();   }
    delay(900);

    gamesTotal++; affAdd(AFF_GAME);
    prefs.putInt("games", gamesTotal);

    resetGameInput();
    unsigned long t1 = millis();
    while (true) {
      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      centerText(nb ? "NEW BEST!" : "YOUR TIME", 3, 1);
      char b[12]; snprintf(b, sizeof(b), "%d", rt);
      centerText(b, 15, 3);
      centerText("ms", 42, 1);
      if ((millis()/500)%2==0) centerText("TAP retry - HOLD menu", 44, 1);
      display.display();
      GameInput gi = pollGameInput();
      if (gi.tap) break;
      if (gi.quit) return;
      if (millis()-t1 > 20000) return;
      delay(30);
    }
  }
}

// ================================================================
//  GAME 4 : TIMING
// ================================================================
void gameTiming() {
  while (true) {
    resetGameInput();
    countdown("STOP IN THE ZONE");

    const int BX=8, BY=20, BW=112, BH=13;
    int score=0;
    float zw=30, spd=1.6;
    bool dead=false;
    resetGameInput();

    while (!dead) {
      int zx = random(BX+4, BX+BW-(int)zw-4);
      float mk = BX+1;
      int dir = 1;
      bool done = false;

      while (!done) {
        GameInput gi = pollGameInput();
        if (gi.quit) return;

        if (gi.tap) {
          if (mk >= zx && mk <= zx+zw) {
            score++;
            sfxCombo(min(score,6));
            for (int f=0;f<2;f++) {
              display.fillRect(zx, BY, (int)zw, BH, SH110X_WHITE);
              display.display(); delay(55);
              display.fillRect(zx, BY, (int)zw, BH, SH110X_BLACK);
              display.display(); delay(55);
            }
            zw -= 2.2; if (zw < 7) zw = 7;
            spd += 0.28; if (spd > 6.5) spd = 6.5;
            done = true;
          } else { sfxError(); dead = true; done = true; }
          continue;
        }

        mk += spd*dir;
        if (mk >= BX+BW-2) { mk = BX+BW-2; dir = -1; }
        if (mk <= BX+1)    { mk = BX+1;    dir =  1; }

        display.clearDisplay();
        display.setTextColor(SH110X_WHITE);
        centerText("TIMING", 2, 1);
        display.drawRect(BX, BY, BW, BH, SH110X_WHITE);
        for (int x=zx; x<zx+(int)zw; x++)
          for (int y=BY+2; y<BY+BH-2; y++)
            if ((x+y)%2==0) display.drawPixel(x, y, SH110X_WHITE);
        display.drawFastVLine(zx, BY, BH, SH110X_WHITE);
        display.drawFastVLine(zx+(int)zw, BY, BH, SH110X_WHITE);
        display.fillRect((int)mk-1, BY-4, 3, BH+8, SH110X_WHITE);
        display.setCursor(2, 40);  display.print("Score "); display.print(score);
        display.setCursor(84, 40); display.print("HI ");    display.print(hsTiming);
        display.display();
        delay(16);
      }
    }

    sfxLose();
    bool nb = score > hsTiming;
    if (nb) { hsTiming = score; prefs.putInt("hsTiming", hsTiming); }
    if (!gameOver(score, hsTiming, nb)) return;
  }
}

// ================================================================
//  GAME 5 : ROCK PAPER SCISSORS
// ================================================================
void drawRPS(int c, int cx, int cy) {
  if (c == 0) {
    display.fillCircle(cx, cy, 12, SH110X_WHITE);
    display.fillRect(cx-12, cy, 24, 9, SH110X_WHITE);
    display.drawFastHLine(cx-6, cy-3, 5, SH110X_BLACK);
    display.drawFastHLine(cx+2, cy-3, 5, SH110X_BLACK);
  } else if (c == 1) {
    display.fillRect(cx-12, cy-15, 24, 30, SH110X_WHITE);
    for (int i=1;i<4;i++)
      display.drawFastHLine(cx-9, cy-15+i*8, 18, SH110X_BLACK);
  } else {
    display.drawLine(cx-6, cy+14, cx+6, cy-14, SH110X_WHITE);
    display.drawLine(cx-5, cy+14, cx+7, cy-14, SH110X_WHITE);
    display.drawLine(cx+6, cy+14, cx-6, cy-14, SH110X_WHITE);
    display.drawLine(cx+7, cy+14, cx-5, cy-14, SH110X_WHITE);
    display.fillCircle(cx-6, cy+14, 4, SH110X_WHITE);
    display.fillCircle(cx+6, cy+14, 4, SH110X_WHITE);
    display.fillCircle(cx-6, cy+14, 2, SH110X_BLACK);
    display.fillCircle(cx+6, cy+14, 2, SH110X_BLACK);
  }
}

void gameRPS() {
  const char* NM[] = { "ROCK","PAPER","SCISSORS" };
  int w=0, l=0, dr=0;

  while (true) {
    resetGameInput();
    int ch=0; bool lock=false, down=false;
    unsigned long at=0, act=millis();

    while (!lock) {
      bool now = (digitalRead(TOUCH_PIN)==HIGH);
      int hp=0;
      if (now && !down) { down=true; at=millis(); }
      else if (now && down) {
        unsigned long held = millis()-at;
        hp = (int)constrain(map(held,200,1100,0,100),0L,100L);
        if (held >= 1100) { lock=true; sfxSelect();
                            while(digitalRead(TOUCH_PIN)==HIGH) delay(10); break; }
        if (held >= GAME_QUIT_MS+900) return;
      }
      else if (!now && down) {
        unsigned long held=millis()-at; down=false;
        if (held < 1100) { ch=(ch+1)%3; sfxTick(); act=millis(); }
      }

      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      char sc[12]; snprintf(sc,sizeof(sc),"%d-%d",w,l);
      titleBar("YOU vs KITO", sc);
      drawRPS(ch, 26, 34);
      display.setTextSize(1);
      display.setCursor(52, 24); display.print(NM[ch]);
      display.setCursor(52, 38); display.print("TAP change");
      if (hp>0) {
        int bw=map(hp,0,100,0,124);
        display.drawRect(2, 43,124,7,SH110X_WHITE);
        display.fillRect(2, 43,bw,7,SH110X_WHITE);
      } else if ((millis()/500)%2==0) {
        display.setCursor(52, 41); display.print("HOLD lock");
      }
      display.display();
      if (millis()-act > 22000) return;
      delay(16);
    }

    // Kito thinks
    int k=0;
    for (int i=0;i<14;i++) {
      k = random(0,3);
      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      centerText("KITO PICKS...", 2, 1);
      drawRPS(k, 64, 36);
      display.display();
      tone2(1500+i*70, 14);
      delay(65 + i*13);
    }
    k = random(0,3);

    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    display.setTextSize(1);
    display.setCursor(14, 2); display.print("YOU");
    display.setCursor(96, 2); display.print("KITO");
    drawRPS(ch, 24, 34);
    drawRPS(k, 104, 34);
    display.setTextSize(2);
    display.setCursor(56, 28); display.print("VS");
    display.display();
    sfxChirp();
    delay(1000);

    int res;
    if (ch == k) res = 0;
    else if ((ch==0&&k==2)||(ch==1&&k==0)||(ch==2&&k==1)) res = 1;
    else res = 2;

    gamesTotal++; affAdd(AFF_GAME);
    prefs.putInt("games", gamesTotal);

    if (res == 0) {
      dr++;
      sfxHuh();
      playAnim(ANIM_CONFUSE, false);
      showFace(FACE_CONFUSED);
      delay(500);
      display.setTextColor(SH110X_WHITE);
      centerText("DRAW!", 41, 2);
      display.display();
      delay(1200);
    }
    else if (res == 1) {
      // You win -> Kito sulks
      w++; rpsWins++;
      prefs.putInt("rps", rpsWins);
      int r = random(0,3);
      if (r==0)      { sfxCry();   playAnimLoops(ANIM_CRY,1);  showFace(FACE_CRYING); }
      else if (r==1) { sfxAngry(); playAnim(ANIM_FUME,false);  showFace(FACE_ANGRY);  }
      else           { sfxSad();   playAnim(ANIM_SIGH,false);  showFace(FACE_SAD);    }
      delay(1000);
      display.setTextColor(SH110X_WHITE);
      centerText("YOU WIN!", 41, 1);
      display.display();
      delay(1100);
    }
    else {
      // Kito wins -> gloats
      l++;
      sfxWin();
      playAnimLoops(ANIM_LAUGH, 2);
      showFace(FACE_MISCHIEF);
      delay(600);
      showFace(FACE_SUN_GLASSES);
      delay(800);
      display.setTextColor(SH110X_WHITE);
      centerText("KITO WINS!", 41, 1);
      display.display();
      delay(1100);
    }

    resetGameInput();
    unsigned long t0=millis();
    while (true) {
      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      titleBar("SCORE", "");
      display.setTextSize(1);
      display.setCursor(12, 18); display.print("You");
      display.setCursor(12, 30); display.print("Kito");
      display.setCursor(12, 42); display.print("Draw");
      display.setTextSize(2);
      display.setCursor(70, 14); display.print(w);
      display.setCursor(70, 26); display.print(l);
      display.setCursor(70, 38); display.print(dr);
      display.setTextSize(1);
      if ((millis()/500)%2==0) centerText("TAP again - HOLD menu", 45, 1);
      display.display();
      GameInput gi = pollGameInput();
      if (gi.tap) break;
      if (gi.quit) return;
      if (millis()-t0 > 20000) return;
      delay(30);
    }
  }
}

// ================================================================
//  GAME 6 : COIN FLIP
// ================================================================
void drawCoin(int cx, int cy, int rx, bool heads) {
  if (rx < 2) { display.drawFastVLine(cx, cy-14, 28, SH110X_WHITE); return; }
  for (int a=0;a<360;a+=4) {
    float r = a*PI/180.0;
    display.drawPixel(cx+(int)(cos(r)*rx), cy+(int)(sin(r)*14), SH110X_WHITE);
    display.drawPixel(cx+(int)(cos(r)*(rx-1)), cy+(int)(sin(r)*13), SH110X_WHITE);
  }
  if (rx > 8) {
    display.setTextSize(2);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(cx-5, cy-7);
    display.print(heads ? "H" : "T");
  }
}

void gameCoin() {
  int streak=0;

  while (true) {
    resetGameInput();
    int gs=0; bool lock=false, down=false;
    unsigned long at=0, act=millis();

    while (!lock) {
      bool now=(digitalRead(TOUCH_PIN)==HIGH);
      int hp=0;
      if (now && !down) { down=true; at=millis(); }
      else if (now && down) {
        unsigned long held=millis()-at;
        hp=(int)constrain(map(held,200,1100,0,100),0L,100L);
        if (held>=1100) { lock=true; sfxSelect();
                          while(digitalRead(TOUCH_PIN)==HIGH) delay(10); break; }
        if (held>=GAME_QUIT_MS+900) return;
      }
      else if (!now && down) {
        unsigned long held=millis()-at; down=false;
        if (held<1100) { gs=1-gs; sfxTick(); act=millis(); }
      }

      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      char sb[16]; snprintf(sb,sizeof(sb),"St%d B%d",streak,coinBest);
      titleBar("COIN FLIP", sb);
      centerText("YOUR GUESS", 15, 1);
      display.setTextSize(3);
      const char* g = gs ? "TAILS" : "HEADS";
      int16_t x1,y1; uint16_t w2,h2;
      display.getTextBounds(g,0,0,&x1,&y1,&w2,&h2);
      if ((int)w2 > 124) { display.setTextSize(2);
                           display.getTextBounds(g,0,0,&x1,&y1,&w2,&h2); }
      display.setCursor((128-w2)/2, 26);
      display.print(g);
      display.setTextSize(1);
      if (hp>0) {
        int bw=map(hp,0,100,0,124);
        display.drawRect(2, 44,124,7,SH110X_WHITE);
        display.fillRect(2, 44,bw,7,SH110X_WHITE);
      } else if ((millis()/500)%2==0)
        centerText("TAP switch - HOLD flip", 45, 1);
      display.display();
      if (millis()-act > 22000) return;
      delay(16);
    }

    int res = random(0,2);
    bool sh = true;
    for (int i=0;i<22;i++) {
      float ph = i*0.9;
      int rx = (int)fabs(cos(ph)*20);
      if (rx < 2) sh = !sh;
      int cy = 34 - (int)(14*sin((float)i/22*PI));
      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      centerText("FLIPPING...", 2, 1);
      drawCoin(64, cy, rx, sh);
      display.display();
      tone2(1500+rx*35, 10);
      delay(38 + i*6);
    }
    for (int f=0;f<4;f++) {
      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      drawCoin(64, 32, 20, res==0);
      if (f%2==0) display.drawCircle(64, 32, 24, SH110X_WHITE);
      display.display();
      tone2(2100, 45);
      delay(110);
    }

    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    centerText(res==0 ? "HEADS" : "TAILS", 22, 3);
    display.display();
    delay(850);

    gamesTotal++; affAdd(AFF_GAME);
    prefs.putInt("games", gamesTotal);

    if (res == gs) {
      streak++;
      if (streak > coinBest) { coinBest = streak; prefs.putInt("coin", coinBest); }
      confetti();
      fireworks();
      playAnimLoops(ANIM_CELEBRATE, 2);
      showFace(FACE_EXCITED);
      delay(700);
      display.setTextColor(SH110X_WHITE);
      char b[20]; snprintf(b,sizeof(b),"Streak %d",streak);
      centerText(b, 42, 1);
      display.display();
      delay(1100);
    } else {
      streak = 0;
      sfxLose();
      playAnim(ANIM_SIGH, false);
      showFace(FACE_DROOPY);
      delay(600);
      for (int f=0;f<4;f++) {
        display.clearDisplay();
        display.setTextColor(SH110X_WHITE);
        centerText("BETTER LUCK", 16, 1);
        centerText("NEXT TIME", 30, 2);
        if (f%2==0) display.drawRect(2, 8, 124, 44, SH110X_WHITE);
        display.display();
        delay(220);
      }
      delay(500);
    }

    resetGameInput();
    unsigned long t0=millis();
    while (true) {
      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      centerText("STREAK", 4, 1);
      char b[8]; snprintf(b,sizeof(b),"%d",streak);
      centerText(b, 16, 3);
      snprintf(b,sizeof(b),"Best %d",coinBest);
      centerText(b, 42, 1);
      if ((millis()/500)%2==0) centerText("TAP flip - HOLD menu", 45, 1);
      display.display();
      GameInput gi=pollGameInput();
      if (gi.tap) break;
      if (gi.quit) return;
      if (millis()-t0 > 20000) return;
      delay(30);
    }
  }
}

// ================================================================
//  GAME 7 : FOCUS  (Pomodoro — Kito works alongside you)
// ================================================================
void gameFocus() {
  const unsigned long WORK = 25UL*60UL*1000UL;
  const unsigned long BRK  =  5UL*60UL*1000UL;

  while (true) {
    resetGameInput();
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    centerText("FOCUS", 3, 1);
    centerText("25", 14, 3);
    centerText("minutes", 42, 1);
    display.display();

    unsigned long t0=millis(); bool go=false;
    while (!go) {
      display.fillRect(0, 42,128,12,SH110X_BLACK);
      if ((millis()/500)%2==0) {
        display.setTextColor(SH110X_WHITE);
        centerText("TAP start - HOLD exit", 44, 1);
      }
      display.display();
      GameInput gi=pollGameInput();
      if (gi.quit) return;
      if (gi.tap) { go=true; break; }
      if (millis()-t0 > 22000) return;
      delay(30);
    }

    sfxSelect();
    showFace(FACE_SERIOUS);
    delay(800);

    unsigned long st = millis();
    bool quit=false;
    while (millis()-st < WORK) {
      unsigned long el=millis()-st, lf=WORK-el;
      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      char sb[10]; snprintf(sb,sizeof(sb),"x%d",pomoTotal);
      titleBar("FOCUS", sb);
      char t[8];
      snprintf(t,sizeof(t),"%02d:%02d",(int)(lf/60000),(int)((lf%60000)/1000));
      centerText(t, 18, 3);
      int w=map(el,0,WORK,0,124);
      display.drawRect(2, 40,124,8,SH110X_WHITE);
      display.fillRect(2, 40,w,8,SH110X_WHITE);
      display.display();

      GameInput gi=pollGameInput();
      if (gi.quit) { quit=true; break; }

      // Kito quietly works too
      if (random(1200) < 3) { playAnim(ANIM_FOCUS, true); }
      delay(180);
    }
    if (quit) { sfxSigh(); return; }

    pomoTotal++;
    prefs.putInt("pomo", pomoTotal);
    bondAdd(1);
    affAdd(10);

    confetti();
    sfxFanfare();
    playAnimLoops(ANIM_CELEBRATE, 2);
    showFace(FACE_PROUD);
    delay(900);

    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    centerText("BREAK TIME", 22, 2);
    display.display();
    sfxHappy();
    delay(1200);

    st = millis();
    while (millis()-st < BRK) {
      unsigned long lf = BRK-(millis()-st);
      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      titleBar("BREAK", "");
      char t[8];
      snprintf(t,sizeof(t),"%02d:%02d",(int)(lf/60000),(int)((lf%60000)/1000));
      centerText(t, 20, 3);
      int w=map(millis()-st,0,BRK,0,124);
      display.drawRect(2, 41,124,6,SH110X_WHITE);
      display.fillRect(2, 41,w,6,SH110X_WHITE);
      display.display();
      GameInput gi=pollGameInput();
      if (gi.quit) return;
      if (random(1200) < 4) playAnim(ANIM_BOUNCE, true);
      delay(180);
    }

    sfxTrill();
    showFace(FACE_HAPPY);
    delay(900);

    resetGameInput();
    unsigned long t1=millis();
    while (true) {
      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      centerText("SESSIONS", 6, 1);
      char b[8]; snprintf(b,sizeof(b),"%d",pomoTotal);
      centerText(b, 18, 3);
      if ((millis()/500)%2==0) centerText("TAP again - HOLD menu", 45, 1);
      display.display();
      GameInput gi=pollGameInput();
      if (gi.tap) break;
      if (gi.quit) return;
      if (millis()-t1 > 20000) return;
      delay(30);
    }
  }
}

// ================================================================
//                      M E N U   ( carousel )
// ================================================================
const char* MI[]  = { "DINO","FLAPPY","REACTION","TIMING","R P S",
                      "COIN","FOCUS","STATS","SETTINGS","BACK" };
const char* MS[]  = { "Jump the cacti","Fly the gaps","How fast are you?",
                      "Stop in the zone","Beat Kito!","Heads or tails?",
                      "Pomodoro timer","Our history","Customize me","Exit" };
const int MCOUNT = 10;

void drawIcon(int i, int cx, int cy) {
  switch (i) {
    case 0:  // dino
      display.fillRect(cx-6, cy-2, 9, 10, SH110X_WHITE);
      display.fillRect(cx-2, cy-9, 8, 8, SH110X_WHITE);
      display.drawPixel(cx+4, cy-6, SH110X_BLACK);
      display.fillRect(cx-11, cy+1, 5, 3, SH110X_WHITE);
      display.fillRect(cx-5, cy+8, 3, 4, SH110X_WHITE);
      display.fillRect(cx+1, cy+8, 3, 4, SH110X_WHITE);
      break;
    case 1:  // bird
      display.fillCircle(cx, cy, 7, SH110X_WHITE);
      display.drawPixel(cx+3, cy-2, SH110X_BLACK);
      display.fillTriangle(cx+6,cy, cx+12,cy-2, cx+12,cy+3, SH110X_WHITE);
      display.fillTriangle(cx-8,cy, cx-2,cy-5, cx-2,cy+4, SH110X_WHITE);
      break;
    case 2:  // bolt
      display.fillTriangle(cx+2,cy-12, cx-6,cy+2, cx+1,cy+2, SH110X_WHITE);
      display.fillTriangle(cx-1,cy+12, cx+7,cy-2, cx+0,cy-2, SH110X_WHITE);
      break;
    case 3:  // timing bar
      display.drawRect(cx-14, cy-5, 28, 11, SH110X_WHITE);
      for (int x=cx-3;x<cx+5;x++)
        for (int y=cy-3;y<cy+4;y++)
          if ((x+y)%2==0) display.drawPixel(x,y,SH110X_WHITE);
      display.fillRect(cx+6, cy-8, 3, 17, SH110X_WHITE);
      break;
    case 4:  // rps
      display.fillCircle(cx-7, cy, 6, SH110X_WHITE);
      display.fillRect(cx-10, cy, 7, 5, SH110X_WHITE);
      display.drawLine(cx+3,cy+6, cx+12,cy-5, SH110X_WHITE);
      display.drawLine(cx+12,cy+6, cx+3,cy-5, SH110X_WHITE);
      display.drawCircle(cx+4, cy+7, 2, SH110X_WHITE);
      display.drawCircle(cx+12, cy+7, 2, SH110X_WHITE);
      break;
    case 5:  // coin
      display.drawCircle(cx, cy, 11, SH110X_WHITE);
      display.drawCircle(cx, cy, 10, SH110X_WHITE);
      display.drawCircle(cx, cy, 7, SH110X_WHITE);
      display.setTextSize(1);
      display.setTextColor(SH110X_WHITE);
      display.setCursor(cx-2, cy-3);
      display.print("K");
      break;
    case 6:  // clock/tomato
      display.drawCircle(cx, cy+1, 12, SH110X_WHITE);
      display.drawCircle(cx, cy+1, 11, SH110X_WHITE);
      display.drawLine(cx, cy+1, cx, cy-6, SH110X_WHITE);
      display.drawLine(cx, cy+1, cx+6, cy+4, SH110X_WHITE);
      display.fillCircle(cx, cy+1, 2, SH110X_WHITE);
      display.fillRect(cx-1, cy-14, 3, 4, SH110X_WHITE);
      break;
    case 7:  // stats — heart + bars
      display.fillCircle(cx-4, cy-6, 4, SH110X_WHITE);
      display.fillCircle(cx+4, cy-6, 4, SH110X_WHITE);
      display.fillTriangle(cx-8,cy-4, cx+8,cy-4, cx,cy+6, SH110X_WHITE);
      display.fillRect(cx-11, cy+9, 4, 5, SH110X_WHITE);
      display.fillRect(cx-4,  cy+6, 4, 8, SH110X_WHITE);
      display.fillRect(cx+3,  cy+2, 4, 12, SH110X_WHITE);
      break;
    case 8: { // gear
      display.drawCircle(cx, cy, 9, SH110X_WHITE);
      display.fillCircle(cx, cy, 4, SH110X_WHITE);
      display.fillCircle(cx, cy, 2, SH110X_BLACK);
      for (int k=0;k<8;k++) {
        float a = k*(PI/4.0);
        int x1=cx+(int)(cos(a)*9),  y1=cy+(int)(sin(a)*9);
        int x2=cx+(int)(cos(a)*14), y2=cy+(int)(sin(a)*14);
        display.drawLine(x1,y1,x2,y2, SH110X_WHITE);
        display.drawLine(x1+1,y1,x2+1,y2, SH110X_WHITE);
      }
      break;
    }
    case 9:  // back
      display.fillTriangle(cx-10,cy, cx+2,cy-8, cx+2,cy+8, SH110X_WHITE);
      display.fillRect(cx+2, cy-3, 10, 7, SH110X_WHITE);
      break;
  }
}

void drawMenu(int idx, float slide, int hp) {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  int ox = (int)slide;

  drawIcon(idx, 24+ox, 32);

  display.setTextSize(2);
  int16_t x1,y1; uint16_t w,h;
  display.getTextBounds(MI[idx], 0,0,&x1,&y1,&w,&h);
  int tx = 48+ox;
  if (48 + (int)w > 126) {
    display.setTextSize(1);
    display.getTextBounds(MI[idx],0,0,&x1,&y1,&w,&h);
    display.setCursor(tx, 24);
  } else display.setCursor(tx, 22);
  display.print(MI[idx]);

  display.setTextSize(1);
  display.setCursor(tx, 40);
  display.print(MS[idx]);

  char pg[8]; snprintf(pg,sizeof(pg),"%d/%d", idx+1, MCOUNT);
  titleBar("MENU", pg);

  // dots
  int tw = MCOUNT*7;
  int sx = (128-tw)/2 + 2;
  for (int i=0;i<MCOUNT;i++) {
    int dx = sx + i*7;
    if (i==idx) display.fillCircle(dx, 58, 2, SH110X_WHITE);
    else        display.drawPixel(dx, 58, SH110X_WHITE);
  }

  if ((millis()/600)%2==0)
    display.fillTriangle(124,30, 119,26, 119,34, SH110X_WHITE);

  if (hp > 0) {
    int w2 = map(hp, 0, 100, 0, 124);
    display.drawRect(2, 41, 124, 5, SH110X_WHITE);
    display.fillRect(2, 41, w2, 5, SH110X_WHITE);
  }
  display.display();
}

int runMenu() {
  int idx = 0;
  float slide = 0;
  unsigned long act = millis();
  bool down=false; unsigned long at=0;

  sfxSelect();
  resetGameInput();

  while (true) {
    slide += (0 - slide) * 0.30;
    if (fabs(slide) < 0.6) slide = 0;

    bool now = (digitalRead(TOUCH_PIN)==HIGH);
    int hp = 0;

    if (now && !down) { down=true; at=millis(); }
    else if (now && down) {
      unsigned long held = millis()-at;
      hp = (int)constrain(map(held, 250, MENU_SELECT_MS, 0, 100), 0L, 100L);
      if (held >= MENU_SELECT_MS) {
        sfxSelect();
        for (int f=0;f<2;f++) {
          display.clearDisplay();
          display.fillRect(0, 0,128,64,SH110X_WHITE);
          display.setTextColor(SH110X_BLACK);
          display.setTextSize(2);
          int16_t x1,y1; uint16_t w,h;
          display.getTextBounds(MI[idx],0,0,&x1,&y1,&w,&h);
          if ((int)w > 124) { display.setTextSize(1);
                              display.getTextBounds(MI[idx],0,0,&x1,&y1,&w,&h); }
          display.setCursor((128-w)/2, 26);
          display.print(MI[idx]);
          display.display();
          delay(70);
          drawMenu(idx, 0, 100);
          delay(50);
        }
        while (digitalRead(TOUCH_PIN)==HIGH) delay(10);
        delay(130);
        if (idx == MCOUNT-1) return -1;
        return idx;
      }
    }
    else if (!now && down) {
      unsigned long held = millis()-at;
      down = false;
      if (held < MENU_SELECT_MS) {
        idx = (idx+1) % MCOUNT;
        slide = 128;
        sfxTick();
        act = millis();
      }
    }
    if (now) act = millis();

    drawMenu(idx, slide, hp);
    if (millis()-act > MENU_TIMEOUT) { sfxBack(); return -1; }
    delay(16);
  }
}

// ================================================================
//                        P O W E R
// ================================================================
void wakeUp() {
  display.oled_command(SH110X_DISPLAYON);
  display.setContrast(brightness);

  sfxBoot();
  playAnim(ANIM_BOOT, false);
  playAnim(ANIM_STRETCH, false);
  playAnim(ANIM_WAKEUP, false);

  // ---- Birthday? ----
  if (isBirthday()) {
    struct tm ti;
    getLocalTime(&ti, 100);
    if (lastBdayYear != ti.tm_year) {
      lastBdayYear = ti.tm_year;
      prefs.putInt("bYr", lastBdayYear);
      birthdayParty();
      greeted = true;
      showResting();
      return;
    }
  }

  // ---- Greeting scaled to the relationship ----
  if (affection < 20) {
    // Been neglected — genuinely sad to have been left
    sfxSad();
    showFace(FACE_SAD);
    delay(1000);
    playAnim(ANIM_SIGH, false);
    display.setTextColor(SH110X_WHITE);
    centerText("you came back...", 43, 1);
    display.display();
    delay(1300);
  }
  else if (affection < 40) {
    sfxHuh();
    showFace(FACE_MEH);
    delay(900);
    playAnim(ANIM_GLANCE, false);
  }
  else if (bond >= 8) {
    sfxLove();
    playAnimLoops(ANIM_LOVEBURST, 1);
    showFace(FACE_LOVE);
    delay(1100);
  }
  else if (bond >= 4) {
    sfxTrill();
    playAnim(ANIM_BOUNCE, false);
    showFace(FACE_HAPPY);
    delay(900);
  }
  else {
    sfxChirp();
    playAnim(ANIM_SHY, false);
  }

  greeted = true;
  showResting();
  nextMoodEvent = millis() + random(3000, 7000);
}

void goSleep() {
  saveLife();
  sfxSleep();
  playAnim(ANIM_YAWN, false);
  playAnim(ANIM_DRIFT, false);
  showFace(FACE_DREAMMING);
  delay(900);

  for (int c=brightness; c>=0; c-=12) {
    display.setContrast(c);
    delay(22);
  }
  display.oled_command(SH110X_DISPLAYOFF);

  // wait for release so wake arms cleanly
  unsigned long t0 = millis();
  while (digitalRead(TOUCH_PIN)==HIGH) {
    if (millis()-t0 > 3000) break;
    delay(10);
  }
  delay(200);

  ledcWriteTone(BUZZER_PIN, 0);
  prefs.end();

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << TOUCH_GPIO, ESP_GPIO_WAKEUP_GPIO_HIGH);
  esp_deep_sleep_start();
}

// hold 7s to power on
bool confirmPowerOn() {
  unsigned long t0 = millis();
  display.oled_command(SH110X_DISPLAYON);
  display.setContrast(brightness);

  while (digitalRead(TOUCH_PIN)==HIGH) {
    unsigned long held = millis()-t0;
    display.clearDisplay();
    display.setTextColor(SH110X_WHITE);
    centerText(kitoName, 16, 2);
    int w = map(constrain(held,0UL,(unsigned long)SLEEP_MS), 0, SLEEP_MS, 0, 100);
    display.drawRoundRect(13, 44, 102, 10, 3, SH110X_WHITE);
    display.fillRoundRect(14, 45, w, 8, 2, SH110X_WHITE);
    display.display();
    if (held >= SLEEP_MS) return true;
    delay(20);
  }
  display.oled_command(SH110X_DISPLAYOFF);
  return false;
}

// ================================================================
//  SH1106 FULL RAM WIPE
//  The SH1106 has 132 columns of internal RAM but only 128 are shown.
//  Adafruit's clearDisplay() only clears the 128 visible ones, leaving
//  junk in the hidden columns + un-inited pages -> stray blocks and
//  vertical lines at the edges. This wipes ALL 132 x 8 pages directly.
// ================================================================
void sh1106HardWipe() {
  for (uint8_t page = 0; page < 8; page++) {
    // set page address
    Wire.beginTransmission(OLED_ADDR);
    Wire.write(0x00);              // command stream
    Wire.write(0xB0 | page);       // page 0..7
    Wire.write(0x00);              // lower column = 0
    Wire.write(0x10);              // higher column = 0
    Wire.endTransmission();

    // write 132 zero bytes across the whole page (all columns incl. hidden)
    for (uint8_t col = 0; col < 132; ) {
      Wire.beginTransmission(OLED_ADDR);
      Wire.write(0x40);            // data stream
      for (uint8_t i = 0; i < 16 && col < 132; i++, col++) {
        Wire.write(0x00);
      }
      Wire.endTransmission();
    }
  }
}

// ================================================================
//                          S E T U P
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(TOUCH_PIN, INPUT);
  randomSeed(esp_random());

  ledcAttach(BUZZER_PIN, 2000, 8);
  ledcWriteTone(BUZZER_PIN, 0);

  Wire.begin(KITO_SDA, KITO_SCL);
  Wire.setClock(400000);          // 400kHz — reliable on 1.3" panels + long wires
  display.begin(OLED_ADDR, true);

  // ---- Kill SH1106 edge artifacts: wipe ALL 132 columns x 8 pages ----
  sh1106HardWipe();
  display.clearDisplay();
  display.display();

  // ---------- LOAD EVERYTHING (survives hard reboot) ----------
  prefs.begin("kito", false);

  String nm = prefs.getString("name", "Kito");
  strncpy(kitoName, nm.c_str(), sizeof(kitoName)-1);

  bond       = prefs.getInt("bond",  0);
  affection  = prefs.getInt("aff",   60);
  savedMood  = prefs.getInt("mood",  0);
  sessions   = prefs.getInt("sess",  0);
  gamesTotal = prefs.getInt("games", 0);
  petsTotal  = prefs.getInt("pets",  0);
  rubsTotal  = prefs.getInt("rubs",  0);
  awakeMinTotal = prefs.getULong("awake", 0);
  bDay       = prefs.getInt("bday",  0);
  bMon       = prefs.getInt("bmon",  0);
  lastBdayYear = prefs.getInt("bYr", 0);

  soundOn     = prefs.getBool("snd",  true);
  volumeLevel = prefs.getInt("vol",   2);
  brightness  = prefs.getInt("brt",   255);
  idleChatty  = prefs.getBool("chat", true);
  clock24h    = prefs.getBool("c24",  false);

  hsDino   = prefs.getInt("hsDino",   0);
  hsFlappy = prefs.getInt("hsFlappy", 0);
  hsReact  = prefs.getInt("hsReact",  9999);
  hsTiming = prefs.getInt("hsTiming", 0);
  rpsWins  = prefs.getInt("rps",  0);
  coinBest = prefs.getInt("coin", 0);
  pomoTotal= prefs.getInt("pomo", 0);

  display.setContrast(brightness);

  // ---------- Did the RTC survive? ----------
  {
    struct tm ti;
    timeOK = (getLocalTime(&ti, 200) && ti.tm_year >= 120);
  }

  // ---------- New session ----------
  sessions++;
  prefs.putInt("sess", sessions);
  sessionStart = millis();

  // Affection decays for time apart — the needs loop
  if (sessions > 1) {
    affection = constrain(affection - AFF_SESSION_DECAY, 0, AFF_MAX);
    prefs.putInt("aff", affection);
  }

  mood = (Mood)savedMood;   // mood persists across boots

  esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();

  if (c == ESP_SLEEP_WAKEUP_GPIO) {
    if (confirmPowerOn()) wakeUp();
    else goSleep();
  } else {
    // cold boot
    display.oled_command(SH110X_DISPLAYOFF);
    while (true) {
      if (digitalRead(TOUCH_PIN)==HIGH && confirmPowerOn()) break;
      delay(20);
    }
    wakeUp();

    // ---- First time ever? Ask for a name. ----
    if (prefs.getString("name", "") == "") {
      display.clearDisplay();
      display.setTextColor(SH110X_WHITE);
      centerText("hello!", 12, 2);
      centerText("what's my name?", 40, 1);
      display.display();
      sfxCurious();
      delay(1800);
      screenSetName();
    }
  }

  lastTouchAt   = millis();
  nextMoodEvent = millis() + random(3000, 7000);
  nextIdleDecay = millis() + (30UL*60UL*1000UL);
}

// ================================================================
//                          L O O P
// ================================================================
void loop() {
  moodTick();

  if (digitalRead(TOUCH_PIN) == HIGH) {
    Gesture g = readGesture();

    switch (g) {

      case G_TAP:
        reactTap();
        break;

      case G_RUB:
        reactRub();
        break;

      case G_DOUBLE: {
        lastTouchAt = millis();
        int sel = runMenu();
        if      (sel == 0) gameDino();
        else if (sel == 1) gameFlappy();
        else if (sel == 2) gameReaction();
        else if (sel == 3) gameTiming();
        else if (sel == 4) gameRPS();
        else if (sel == 5) gameCoin();
        else if (sel == 6) gameFocus();
        else if (sel == 7) screenStats();
        else if (sel == 8) screenSettings();

        saveLife();
        resetGameInput();
        lastTouchAt   = millis();
        showResting();
        nextMoodEvent = millis() + random(3000, 7000);
        break;
      }

      case G_TRIPLE:
        lastTouchAt = millis();
        screenClock();
        nextMoodEvent = millis() + random(3000, 7000);
        break;

      case G_SLEEP:
        goSleep();
        break;

      default: break;
    }
  }

  delay(12);
}
