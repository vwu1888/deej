#include <Adafruit_NeoPixel.h>
#include <Keyboard.h>
#include <EEPROM.h>

// buttonData has a bool for whether a button was pressed,
// int for the last successfully debounced state,
// int for the last detected state change, and an
// unsigned long for the time since a state change
struct buttonData {
  bool toggle;
  int currentState;
  int lastReading;
  unsigned long lastDebounceTime;
};

// encoder contains an encoder's DT and CLK pin states,
// it's DT and CLK state, along with the previous CLK state
// it's rotation, and a counter for how many turns CW minus the turns CCW
struct encoder {
  const int CLKpin;
  const int DTpin;
  int DTstate;
  int CLKstate;
  int prevCLK;
  int rotation;
  int counter;
};

#define LED_PIN 13 // pin the neopixels are wired to
#define LED_COUNT 6 // number of neopixels
#define BRIGHTNESS 100 // brightness of the neopixels
#define DFLT neopixels.Color(50, 0, 255) // RGB values for default volume color
#define MUTE neopixels.Color(0, 90, 255) // RGB values for muted volume color
#define MAX neopixels.Color(255, 255, 255) // RGB values for maxium volume color
Adafruit_NeoPixel neopixels(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800); // create instance of Adafruit_NeoPixels

#define NUM_CHANNELS 6 // number of volume controls
#define NUM_BUTTON_MACROS 3 // number of macro buttons for other controls (such as pause/play)
#define NUM_ENC_MACROS 1 // number of encoders for macros (switches for the encoders should be consider a button macro)

// data for all volume encoders. First value of each element is the CLK pin and the second value is the DT pin
encoder volEncoders[NUM_CHANNELS] = {{2, 3}, {5, 7}, {9, 10}, {11, 12}, {A4, A5}, {A2, A3}}; 
int volEncEEPROMaddrs[NUM_CHANNELS] = {0, 1, 2, 3, 4, 5}; // EEPROM address for each of the volume encoders
int lastEEPROMupdate; // time since the last EEPROM update
int volumeValues[NUM_CHANNELS]; // values sent to the computer to set the volume

// data for all macro encoders. First value of each element is the CLK pin and the second value is the DT pin
encoder macroEncoders[NUM_ENC_MACROS] = {{A1, A0}}; 

// toggles true/false if button was pressed
buttonData macroStates[NUM_BUTTON_MACROS];
buttonData muteStates[NUM_CHANNELS];
int muteEEPROMaddrs[NUM_CHANNELS] = {6, 7, 8, 9, 10, 11};
bool prevMuteToggle[NUM_CHANNELS]; // last change in toggle in a muteStates element

#define ROWS 3 // how many rows in the button matrix
#define COLS 3 // how many columns in the button matrix

int rowPins[ROWS] = {4, 6, 8}; // pins the rows of the button matrix are connected to
int colPins[COLS] = {14, 16, 15}; // pins the columns of the button matrix are connected to
// key for button matrix
buttonData *matrixKey[ROWS][COLS] = {
  {&macroStates[0], &macroStates[1], &macroStates[2]},
  {&muteStates[0], &muteStates[1], &muteStates[2]},
  {&muteStates[3], &muteStates[4], &muteStates[5]},
};

#define debounceDelay 50 // delay between changes in the state of the button
#define EEPROMdelay 30000 // delay between EEPROM updates

void setup() {
  // configures pins for button matrix
  for (int i = 0; i < ROWS; i++) {
    pinMode(rowPins[i], OUTPUT);
  }
  for (int i = 0; i < COLS; i++) {
    pinMode(colPins[i], INPUT_PULLUP);
  }

  // configures neopixels
  pinMode(LED_PIN, OUTPUT);
  neopixels.begin();
  neopixels.fill(DFLT);
  neopixels.setBrightness(BRIGHTNESS);

  // configures encoders for macros
  for (int i = 0; i < NUM_ENC_MACROS; i++) {
    pinMode(macroEncoders[i].CLKpin, INPUT);
    pinMode(macroEncoders[i].DTpin, INPUT);
  }

  // configurs inputs for volume channels
  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(volEncoders[i].CLKpin, INPUT);
    pinMode(volEncoders[i].DTpin, INPUT);
    volEncoders[i].prevCLK = digitalRead(volEncoders[i].CLKpin);
    volEncoders[i].counter = EEPROM.read(volEncEEPROMaddrs[i]);
    muteStates[i].toggle = EEPROM.read(muteEEPROMaddrs[i]);
    if (volEncoders[i].counter == 0) {
      muteLED(i);
    }
  }

  // start seial, keyboard, and shows neopixels
  neopixels.show();
  Serial.begin(500000);
  Keyboard.begin();
}

void loop() {
  // looks for changes in inputs
  updateButtons();
  updateEncoders();
  // computes data given by inputs
  runMacros();
  getVolume();
  sendVolumeValues();
  // updates encVolume values in EEPROM
  updateEEPROM();
}

// checks buttons for change
void updateButtons() {
  // checks each row one by one and looks for a col that coincides
  for (int r = 0; r < ROWS; r++) {
    digitalWrite(rowPins[r], LOW);
    checkCols(r);
    digitalWrite(rowPins[r], HIGH);
  }
}

// checks each col
void checkCols(int r) {
  for (int c = 0; c < COLS; c++) {
    if (debounceButton(r, c)) {
      matrixKey[r][c]->toggle = !matrixKey[r][c]->toggle;
    }
  }
}

// confirms if button was pressed
bool debounceButton(int r, int c) {
  bool isPress = false;
  int reading = digitalRead(colPins[c]);

  if (reading != matrixKey[r][c]->lastReading) {
    matrixKey[r][c]->lastDebounceTime = millis();
  }
  if ((millis() - matrixKey[r][c]->lastDebounceTime) > debounceDelay && reading != matrixKey[r][c]->currentState) {
    matrixKey[r][c]->currentState = reading;
    isPress = reading == LOW;
  }

  matrixKey[r][c]->lastReading = reading;
  return isPress;
}

// checks encoders for change
void updateEncoders() {
  updateVolEncoders();
  updateMacroEncoders();
}

// updates each volume encoders values
void updateVolEncoders() {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    decodeEnc(volEncoders[i]);

    if (volEncoders[i].rotation != 0) {
      getEncoderVol(volEncoders[i], i);
      muteStates[i].toggle = false;
    }

    if (muteStates[i].toggle != prevMuteToggle[i]){
      prevMuteToggle[i] = muteStates[i].toggle;
      if (muteStates[i].toggle) {
        muteLED(i);
        continue;
      }
      defaultLED(i);
    }
  }
}

// updates each macro encoders values
void updateMacroEncoders() {
  for (int i = 0; i < NUM_ENC_MACROS; i++) {
    decodeEnc(macroEncoders[i]);
  }
}

// decodes the encoder's input
void decodeEnc(encoder &enc) {
  getEncState(enc);
  enc.rotation = 0;
  if (enc.CLKstate != enc.prevCLK && enc.CLKstate == 1) {
    getRotation(enc);
  }
  enc.prevCLK = enc.CLKstate;
}

// retrieves the current state of an encoders CLK and DT pin
void getEncState(encoder &enc) {
  enc.CLKstate = digitalRead(enc.CLKpin);
  enc.DTstate = digitalRead(enc.DTpin);
}

// determins the rotation of an encoder
void getRotation(encoder &enc) {
  if (enc.CLKstate == enc.DTstate) {
    enc.rotation = -1;
    return;
  }
  enc.rotation = 1;
}

// increments the volume encoder counter values based on the rotation of the encoder
// and set the LEDs accordingly
void getEncoderVol(encoder &enc, int i) {
  enc.counter += enc.rotation;

  if (enc.counter >= 100) {
    enc.counter = 100;
    maxVolLED(i);
    return;
  }

  if (enc.counter <= 0) {
    enc.counter = 0;
    muteLED(i);
    return;
  }

  if (enc.counter == 1) {
    defaultLED(i);
    return;
  }
}

// converts the values from the encoders to the values sent to the serial monitor
// volume is set to 0 if muteStates is true
void getVolume() {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    if (muteStates[i].toggle) {
      volumeValues[i] = 0;
      continue;
    }
    // converts from 0-100 to 0-1023
    volumeValues[i] = ceil(volEncoders[i].counter * 10.23);
  }
}

// sets specified neopixel to DFLT color
void defaultLED(int i) {
  neopixels.setPixelColor(i, DFLT);
  neopixels.show();
}

// alternates specified neopixel between MAX and DFLT twice
void maxVolLED(int i) {
  for (int c = 0; c < 2; c++) {
    neopixels.setPixelColor(i, MAX);
    neopixels.show();
    delay(175);

    neopixels.setPixelColor(i, DFLT);
    neopixels.show();
    delay(175);
  }
}

// sets specified neopixel to MUTE color
void muteLED(int i) {
  neopixels.setPixelColor(i, MUTE);
  neopixels.show();
}

// runs the macros
void runMacros() {
  runButtonMacros();
  runEncMacros();
}

// runs the macros assigned to buttons
void runButtonMacros() {
  for (int i = 0; i < NUM_BUTTON_MACROS; i++) {
    if (!macroStates[i].toggle) {
      continue;
    }

    switch (i) {
      case 0:
        skipBack(); // button 1
        break;
      case 1:
        pausePlay(); // button 2
        break;
      case 2:
        skipForward(); // button 3
        break;
    }
    macroStates[i].toggle = false;
  }
}

// runs the macros assigned to encoders
void runEncMacros() {
  for (int i = 0; i < NUM_ENC_MACROS; i++) {
    switch (macroEncoders[i].rotation) {
      case -1:
        scrubBackward();// counter-clockwise
        break;
      case 1:
        scrubForward(); // clockwise
        break;
      case 0: break; // do nothing
    }
    macroEncoders[i].rotation = 0;
  }
}

// goes back one media file (ex: go back one song or video)
void skipBack() {
  Keyboard.press(KEY_LEFT_CTRL);
  Keyboard.press(KEY_LEFT_ALT);
  Keyboard.press(KEY_LEFT_ARROW);
  Keyboard.releaseAll();
}

// pause/play media
void pausePlay() {
  Keyboard.press(KEY_LEFT_CTRL);
  Keyboard.press(KEY_LEFT_ALT);
  Keyboard.press(KEY_DOWN_ARROW);
  Keyboard.releaseAll();
}

// goes forward one media file (ex: go forward one song or video)
void skipForward() {
  Keyboard.press(KEY_LEFT_CTRL);
  Keyboard.press(KEY_LEFT_ALT);
  Keyboard.press(KEY_RIGHT_ARROW);
  Keyboard.releaseAll();
}

// move backward in video
void scrubBackward() {
  Keyboard.press(KEY_LEFT_ARROW);
  Keyboard.releaseAll();
}

// move forward in video
void scrubForward() {
  Keyboard.press(KEY_RIGHT_ARROW);
  Keyboard.releaseAll();
}

// updates EEPROM with new encoder volume values every 30 seconds
void updateEEPROM() {
  if (millis() -  lastEEPROMupdate < EEPROMdelay) {
    return;
  }

  for (int i = 0; i < NUM_CHANNELS; i++) {
    EEPROM.update(volEncEEPROMaddrs[i], volEncoders[i].counter);
    EEPROM.update(muteEEPROMaddrs[i], muteStates[i].toggle);
  }
  lastEEPROMupdate = millis();
}

// send volume values to seial monitor
void sendVolumeValues() {
  String builtString = String("");

  for (int i = 0; i < NUM_CHANNELS; i++) {
    builtString += String((int)volumeValues[i]);

    if (i < NUM_CHANNELS - 1) {
      builtString += String("|");
    }
  }

  Serial.println(builtString);
}

// for debugging purposes
void printVolumeValues() {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    String printedString = String("Slider #") + String(i + 1) + String(": ") + String(volumeValues[i]) + String(" mV");
    Serial.write(printedString.c_str());

    if (i < NUM_CHANNELS - 1) {
      Serial.write(" | ");
    } else {
      Serial.write("\n");
    }
  }
}
