/*
 * Author: Fahd Al Riachi - 2023, email: riachi@gmail.com
 * LA CABINE 134 - Telephone installation
 * This code operates an old rotary phone allownig playback and recording 
 * In addition, it allows customizing and tweeking various phone settings
 */

 //updates: Adapted in portvendre to:
 // add preRingInterval, default at 30 secs, with saving mechanism using command 8
 // extend stallPeriod to have 1 min included


#include <Audio.h>
#include <Wire.h>
#include <SD.h>
#include <SPI.h>
#include <TimeLib.h>
#include <SerialFlash.h>
#include <Bounce2.h>
#include <EEPROM.h>


// ===== Define Hardware ===== //
#define SDCARD_CS_PIN    BUILTIN_SDCARD
#define SDCARD_MOSI_PIN  11  
#define SDCARD_SCK_PIN   13
#define MOT 24    // motion sensor (A10)
#define SWM 25    // mode rocker switch (A11)
#define SWH 26    // hook switch (A12)
#define SWD 27    // dial switch (A13)
#define SWN 28    // number pulse switch
#define SWI 33    // on-board internal switch
#define VOL 15    // volume knob (A1)
#define GLED 29   // green led
#define RLED 30   // red led
#define RNGL 36   // ringer left
#define RNGR 37   // ringer right
#define RAND 41   // Random seed

// ===== Define Software ===== //
#define PLAYLISTMAXSIZE   100
#define RECODINGSMAXSIZE  1000
#define ADDR_EEPROM_MARKER 0  // EEPROM marker address
#define MARKER_EEPROM 123     // EEPROM marker value
#define MAX_RINGS 7     // EEPROM marker value

#define RESTART_ADDR       0xE000ED0C
#define READ_RESTART()     (*(volatile uint32_t *)RESTART_ADDR)
#define WRITE_RESTART(val) ((*(volatile uint32_t *)RESTART_ADDR) = (val))


// ======== Variables ======== //

// control vars
String city[3] = {"/PortVendres","/Saida","/Miami"};
byte cityIndex = 0;
String playlist[PLAYLISTMAXSIZE];
boolean didReadConfig;
int playlistSize = 0;
int playNextIndex = 0;
String currentInterview = "";
unsigned long lastRingTime;
int ringsBeforeAbandon = MAX_RINGS;
unsigned long previousFlashMillis = 0;
const long flashInterval = 500;
unsigned int preRingInterval = 5000;
unsigned long motionDetectedTime = 0;
int numberPulseCount = 0;
boolean recEnabled = true;
boolean ringerEnabled = true;
boolean interviewsSelected = true;
boolean prePlayPrompt = true;
boolean autoRestart = true;
String *selectedList = playlist;
int selectedListSize = 0;
unsigned int stallPeriod = 15000;
boolean playing = false;
time_t timeNow;
int autoRestartHour = 3;
int motion; 
float dura = 0;

//Recording vars
String recordingslist[RECODINGSMAXSIZE];
int recordingsListSize = 0;
unsigned long ChunkSize = 0L;
unsigned long Subchunk1Size = 16;
unsigned int AudioFormat = 1;
unsigned int numChannels = 1;
unsigned long sampleRate = 44100;
unsigned int bitsPerSample = 16;
unsigned long byteRate = sampleRate*numChannels*(bitsPerSample/8);// samplerate x channels x (bitspersample / 8)
unsigned int blockAlign = numChannels*bitsPerSample/8;
unsigned long Subchunk2Size = 0L;
unsigned long recByteSaved = 0L;
unsigned long NumSamples = 0L;
byte byte1, byte2, byte3, byte4;
const int myInput = AUDIO_INPUT_MIC;
File frec;
unsigned long previousRecMillis = 0;
unsigned long recInterval = 120000; // 2 mins = 120000
int recNeededSize = 11;

 
boolean status;
AudioControlSGTL5000     audioBoard;

// Audio output
AudioPlaySdWav           playWav1;
AudioOutputI2S           audioOutput;

// send same channel to both Left and right
//AudioMixer4 mixer1;
//AudioConnection patchCord1(playWav1, 0, mixer1, 0);
//AudioConnection patchCord2(mixer1, 0, audioOutput, 0);
//AudioConnection patchCord3(mixer1, 0, audioOutput, 1);

// send stereo file
AudioConnection          patchCord1(playWav1, 0, audioOutput, 0);
AudioConnection          patchCord2(playWav1, 1, audioOutput, 1);

// Audio input
AudioInputI2S            audioInput;
AudioRecordQueue         queue1;
AudioConnection          patchCord4(audioInput, 0, queue1, 0);



const int bounceTime = 6;
typedef enum {Stalling, Idle, PreRinging, Ringing, PrePlay, Playing, RecPrompt, Recording, EndPrompt, Wait, AdminIdle, Command, PlayNext} stateType;
stateType state = Idle;

Bounce hookSwitch = Bounce();
Bounce dialSwitch = Bounce();
Bounce numberSwitch = Bounce();
Bounce modeSwitch = Bounce();
Bounce internalSwitch = Bounce();



//==================================== Setup ==================================== //

void setup() {

  // Set the Time library to use Teensy 3.0's RTC to keep time
  setSyncProvider(getTeensyTime);

  // Attach buttons
  hookSwitch.attach( SWH ,  INPUT_PULLUP );
  dialSwitch.attach( SWD ,  INPUT_PULLUP );
  numberSwitch.attach( SWN ,  INPUT_PULLUP );
  modeSwitch.attach( SWM ,  INPUT_PULLUP );
  internalSwitch.attach( SWI ,  INPUT_PULLUP );

  hookSwitch.interval(bounceTime);
  dialSwitch.interval(bounceTime);
  numberSwitch.interval(bounceTime);
  modeSwitch.interval(bounceTime);
  internalSwitch.interval(bounceTime);

  pinMode(MOT, INPUT);
  pinMode(RLED, OUTPUT);
  pinMode(GLED, OUTPUT);
  pinMode(RNGL, OUTPUT);
  pinMode(RNGR, OUTPUT);
  digitalWrite(RNGL,LOW);
  digitalWrite(RNGR,LOW);
  
  
  // Begine Serial
  Serial.begin(9600);
  delay(1000);
  Serial.println(F("=================== INITIALIZE =================="));
  digitalWrite(RLED, HIGH);

  initializeEEPROM();

  // Configure Audio board
  AudioMemory(60);
  audioBoard.enable();
  audioBoard.inputSelect(myInput);
  audioBoard.micGain(20);  //0-63
  audioBoard.volume(0.7);

  // Configure SPI for SD
  SPI.setMOSI(SDCARD_MOSI_PIN);
  SPI.setSCK(SDCARD_SCK_PIN);
    
  // Open the SD card
  status = SD.begin(SDCARD_CS_PIN);
  if (status) {
    Serial.println(F("- SD library is able to access the filesystem"));
  } else {
    Serial.println(F("ERROR: SD library can not access the filesystem!"));
    stop();
  }

  // Check RTC time status
  if (timeStatus()!= timeSet) {
    Serial.println("ERROR: Unable to sync with the RTC");
  } else {
    Serial.print("- RTC time is running: ");
    Serial.println(getDateTime());
  }
  
  Serial.print(F("- Code was compiled: "));
  Serial.println(__FILE__ ", " __DATE__ ", " __TIME__);
  
  // Get card info and capacity and print to serial
  checkSd();

  log("Restarted","SD & RTC OK");

  logSettings();
  
  Serial.println(F("================= LOAD PLAYLIST ================="));

  Serial.print("- Max allowed interviews: ");
  Serial.print(PLAYLISTMAXSIZE);
  Serial.print(", Max allowed Recordings: ");
  Serial.print(RECODINGSMAXSIZE);
  Serial.print(" of ");
  Serial.print(recInterval/1000);
  Serial.println(" seconds each");
  
  hasEnoughSpaceForMb(recNeededSize);

  playlistSize = fillPlayList(); 
  if (playlistSize > 0){
    Serial.print("    => ");
    Serial.print(playlistSize);
    Serial.println(F(" files loaded into Playlist"));
    log("Playlist size",String(playlistSize) + " / " + String(PLAYLISTMAXSIZE));
    }
  else{
   Serial.println(F("ERROR: no files were found in /Interviews. (files must start with 'INT_' and end with '.wav', mono 16bit, 44.1khz)"));
   log("ERROR","Empty play list");
   stop();
  }

  recordingsListSize = fillRecordingsList();
  Serial.print("    => ");
  Serial.print(recordingsListSize);
  Serial.println(F(" files loaded into Recordings list"));
  log("Recordings list size", String(recordingsListSize) + " / " + String(RECODINGSMAXSIZE));

  unsigned long rs = analogRead(RAND);
  randomSeed(rs);
  Serial.print("- Random seed: ");
  Serial.println(rs);

  Serial.print("- Auto-restart enabled: ");
  Serial.print(autoRestart);
  Serial.print(" @ hour 0");
  Serial.print(autoRestartHour);
  Serial.println(":00:00");
    
  
  Serial.println(F("====================== RUN ======================"));

    
  if (digitalRead(SWM) == HIGH){
    Serial.println("Admin idle");
    state = AdminIdle;
  }
  else
  {
     if(digitalRead(SWH)== LOW){
        state = Wait;
      }
      else {
        state = Idle;
        Serial.println("Idle");
      }
  }

  
}



//================================== Main loop ================================== //

void loop(void) {

  hookSwitch.update();
  dialSwitch.update();
  numberSwitch.update();
  modeSwitch.update();
  internalSwitch.update();
  motion = digitalRead(MOT);
  

    
  switch(state){
    case Stalling: 
    {

        // delay going to idle to give time for people to leave cabine
        playWav1.stop();
        digitalWrite(GLED, LOW);
        digitalWrite(RLED, LOW);

        if(ringerEnabled){
          Serial.print("Stalling for ");
          Serial.print(stallPeriod/1000);
          Serial.println(" seconds...");
          delay(stallPeriod);
        }
        state = Idle;
        Serial.println("Idle after stall");     
        
        break; // from Stalling
    }
    
    case Idle:
    {
      
      playWav1.stop();
      digitalWrite(GLED, HIGH);
      digitalWrite(RLED, LOW);
      ringsBeforeAbandon = MAX_RINGS;
      
      if(hookSwitch.fell()){
        Serial.println("Hook up");
        state = PrePlay;
        delay(1500);
        if (prePlayPrompt){
          playMessage("MSG_PREPLAY.wav");
        }
       
      }

      if(motion == HIGH && ringerEnabled) {
        Serial.println("Motion detected");
        Serial.print("Delay ringing by ");
        Serial.print(preRingInterval/1000);
        Serial.println(" (seconds)");
        motionDetectedTime = millis();
        state = PreRinging;
      }

      if(autoRestart) {
        timeNow = now();
        if(hour(timeNow) == autoRestartHour && minute(timeNow) == 0 && second(timeNow) < 1) {
          String msg = "Scheduled auto-restart time";
          log("Autorestart", msg);
          Serial.println(msg);
          restart();
        }
      }
      
     
      break; // from Idle
    }

    case PreRinging:
    {
      unsigned long now = millis();
      digitalWrite(RLED, HIGH);
      if (now - motionDetectedTime >= preRingInterval){
        digitalWrite(RLED, LOW);
        state = Ringing;       
      }

      if(hookSwitch.fell()){
        Serial.println("Hook lifted during pre-Ring delay");
        state = PrePlay;
        delay(1500);
        if (prePlayPrompt){
          playMessage("MSG_PREPLAY.wav");
        }
      }
       
      break;
    }

    case Ringing:
    {
      int now = millis();
      if (now - lastRingTime > 3000 && ringsBeforeAbandon > 0){
        Serial.println("Ringing");
        for (int j=0;j<2;j++){
          for(int i=0;i<20;i++){
            hookSwitch.update();
            if(hookSwitch.fell()){
              j = 2;
              break;
            }
            digitalWrite(RNGL, i%2);
            digitalWrite(RNGR, 1-(i%2));
            digitalWrite(RLED, i%2);
            digitalWrite(GLED, 1-(i%2));
            delay(20);
          }
        delay(200);
        }
        // stop ringing
        digitalWrite(RNGL, LOW);
        digitalWrite(RNGR, LOW);
        digitalWrite(RLED, LOW);
        digitalWrite(GLED, LOW);
        lastRingTime = now;
        ringsBeforeAbandon--; 
       }


      if(hookSwitch.fell()){
        Serial.println("Answerd");
        state = PrePlay;
        delay(1500);
        if (prePlayPrompt){
          playMessage("MSG_PREPLAY.wav");
        }
      }

      if (ringsBeforeAbandon == 0) {
        log("Ringing abandoned","After " + String(MAX_RINGS) + " rings");
        Serial.println("Abandon Ringing and return to Idle");
        Serial.println("Idle");
        ringsBeforeAbandon = MAX_RINGS;
        state = Idle;
      }
       
      break; // form Ringing
    }

   case PrePlay:
   {
      digitalWrite(GLED, HIGH);
      digitalWrite(RLED, HIGH);
      if(!playWav1.isPlaying()){
        state = Playing;
        currentInterview = pickWavFile();
        log("Playing", currentInterview);
        delay(500);
        playInterview(currentInterview);
      }
   
      break;
   }
   
    case Playing:
    {
      digitalWrite(GLED, LOW);
      digitalWrite(RLED, HIGH);
      float vol = analogRead(VOL);
      vol = vol / 1024;
      audioBoard.volume(vol);
       
      if (playWav1.isPlaying())
      {
         dura = playWav1.lengthMillis();
      }
      else{
        // ended
        Serial.println("Checking if recording is possible...");
        digitalWrite(GLED, LOW);
        digitalWrite(RLED, LOW);
        
        String msg = "Duration(sec): " + String(dura/1000);
        log("Playing ended", msg);
        
        if(canRecord()){
          playMessage("MSG_PREREC.wav");
          state = RecPrompt;
          
        }
        else {
          playMessage("MSG_LEAVE.wav");
          state = EndPrompt;
        }
             
      }

      if(numberSwitch.rose()){
        playWav1.stop();
        state = Playing;
        currentInterview = pickWavFile();
        log("Playing", currentInterview);
        delay(500);
        playInterview(currentInterview);
        
      }
      
      break; // from Playing
    }

   case RecPrompt:
   {
      digitalWrite(GLED, HIGH);
      digitalWrite(RLED, HIGH);
      if(!playWav1.isPlaying()){
        state = Recording;
        previousRecMillis = millis();
        startRecording();
      }
   
      break;
   }

   case Recording:
   {
      digitalWrite(GLED, LOW);
      digitalWrite(RLED, HIGH);
      unsigned long currentMillis = millis();
      continueRecording();
      unsigned long diff = currentMillis - previousRecMillis;

      // 10 seconds before end of recording, start flashing the green led
      if (diff >= (recInterval-10000)) {
        if( (diff/1000 % 2) == 0) {
          digitalWrite(GLED, !digitalRead(GLED));
        }
      }

      // stop recording when recInterval has passed
      if (diff >= recInterval) {
        stopRecording();
        playMessage("MSG_LEAVE.wav");
        state = EndPrompt;
      }
      break;
   }

   case EndPrompt:
   {
      digitalWrite(GLED, HIGH);
      digitalWrite(RLED, HIGH);
      if(!playWav1.isPlaying()){
        state = Wait;
      }
   
      break;
   }

   case Wait:
   {  
      digitalWrite(GLED, LOW);
      unsigned long currentMillis = millis();
      if (currentMillis - previousFlashMillis >= flashInterval) {
        playWav1.stop();
        previousFlashMillis = currentMillis;
        Serial.println("Put hook down!");
        digitalWrite(RLED, !digitalRead(RLED));
      }
   
      break;
   }

   case AdminIdle:
   {
      playWav1.stop();
      digitalWrite(GLED, HIGH);
      digitalWrite(RLED, HIGH);

      if(hookSwitch.fell()){
        Serial.print("> Audition files for ");        
        if(interviewsSelected){
          selectedList = playlist;
          selectedListSize = playlistSize;
          Serial.print("Interviews (");
        }
        else{
          selectedList = recordingslist;
          selectedListSize = recordingsListSize;
          Serial.print("Recordings (number of files: ");
        }

        Serial.print(selectedListSize);
        Serial.println(")");
        
        playNextIndex = -1;
        state = PlayNext;  
      }

      if (modeSwitch.fell()){
        Serial.println("Exit Admin and stall");
        state = Stalling;  
      }

      if (numberSwitch.rose()){
        numberPulseCount++;
        
      }

      if(dialSwitch.rose()){
        Serial.print("  You dialed: ");
        Serial.println(numberPulseCount);
        if (numberPulseCount == 10){
          Serial.println("> Command Mode");
          state = Command;
        }
        else {
          Serial.println("  (Not supported)");
        }
        
        numberPulseCount = 0;
      }
   
      break; // from AdminIdle
   }

   case Command:
   {

    digitalWrite(GLED, LOW);
    digitalWrite(RLED, LOW);
    
    if (numberSwitch.rose()){
        numberPulseCount++;
    }

    if(dialSwitch.rose()){
      digitalWrite(GLED, LOW);
      digitalWrite(RLED, LOW);
      if (numberPulseCount == 1){
        recEnabled = !recEnabled;
        String msg = "  - Rec mode changed to: " + String(recEnabled) + " (1: Enabled, 0: Disabled)";
        Serial.println(msg);
        log("Admin",msg);
        if (recEnabled)
          digitalWrite(GLED, HIGH);
        else
          digitalWrite(RLED, HIGH);
        delay(500);
        writeConfiguration();
      }
      else if (numberPulseCount == 2){
        ringerEnabled = !ringerEnabled;
        String msg = "  - Ringer mode changed to: " + String(ringerEnabled) + " (1: Enabled, 0: Disabled)";
        Serial.println(msg);
        log("Admin",msg);
        if (ringerEnabled)
          digitalWrite(GLED, HIGH);
        else
          digitalWrite(RLED, HIGH);
        delay(500);
        writeConfiguration();
      }
      else if (numberPulseCount == 3){
        prePlayPrompt = !prePlayPrompt;
        String msg = "  - Pre-Play prompt has changed to " + String(prePlayPrompt) + " (1: Enabled, 0: Disabled)";
        Serial.println(msg);
        log("Admin",msg);
        if (prePlayPrompt)
          digitalWrite(GLED, HIGH);
        else
          digitalWrite(RLED, HIGH);
        delay(500);
        writeConfiguration();
      }
      else if (numberPulseCount == 4){
        interviewsSelected = !interviewsSelected;
        String msg = "  - Admin playlist has changed to: " + String(interviewsSelected) + " (1: Interviews, 0: Recordings)";
        Serial.println(msg);
        log("Admin",msg);
        if (interviewsSelected)
          digitalWrite(GLED, HIGH);
        else
          digitalWrite(RLED, HIGH);
        delay(500);
        writeConfiguration();
      }
      else if (numberPulseCount == 5){
        if (stallPeriod == 5000)
          stallPeriod = 10000;
        else if(stallPeriod == 10000)
          stallPeriod = 15000;
        else if(stallPeriod == 15000)
          stallPeriod = 20000;
        else if(stallPeriod == 20000)
          stallPeriod = 30000;
        else if(stallPeriod == 30000)
          stallPeriod = 60000;
        
        else
          stallPeriod = 5000;

        String msg = "  - Stall period changed to: " + String(stallPeriod/1000) + " seconds";
        Serial.println(msg);
        log("Admin",msg);
        if (stallPeriod == 60000) // i.e if equal default
          digitalWrite(GLED, HIGH);
        else
          digitalWrite(RLED, HIGH);
        delay(500);
        writeConfiguration();
      }
      else if (numberPulseCount == 6){ 
        if (recInterval == 30000)
          recInterval = 60000;
        else if(recInterval == 60000)
          recInterval = 120000;
        else if(recInterval == 120000)
          recInterval = 180000;
        else
          recInterval = 30000;
        
        setRecNeededSize();
        String msg =  "  - Recording period changed to: " + String(recInterval/1000) + " sec, each file requiring " + String(recNeededSize) + " MB";
        Serial.println(msg);
        log("Admin",msg);
        if (recInterval == 120000) // i.e if equal default
          digitalWrite(GLED, HIGH);
        else
          digitalWrite(RLED, HIGH);
        delay(500);
        writeLongIntoEEPROM(5, recInterval);
      }
      else if (numberPulseCount == 7){
        cityIndex++;
        if (cityIndex >= getCityArrayLen()){
          cityIndex = 0; // rollover
        }
        String msg = "- City changed to: " + city[cityIndex];
        Serial.println(msg);
        log("Admin",msg);
        
        fillPlayList();
        fillRecordingsList();
        
        if (city[cityIndex] == "/PortVendres") // i.e if equal default
          digitalWrite(GLED, HIGH);
        else
          digitalWrite(RLED, HIGH);
        delay(500);
        writeConfiguration();
      }
      else if (numberPulseCount == 8){

        if (preRingInterval == 5000)
          preRingInterval = 10000;
        else if(preRingInterval == 10000)
          preRingInterval = 20000;
        else if(preRingInterval == 20000)
          preRingInterval = 30000;
        else if(preRingInterval == 30000)
          preRingInterval = 60000;
        
        else
          preRingInterval = 5000;

        String msg = "  - Pre-Ring period changed to: " + String(preRingInterval/1000) + " seconds";
        Serial.println(msg);
        log("Admin",msg);
        if (preRingInterval == 30000) // i.e if equal default
          digitalWrite(GLED, HIGH);
        else
          digitalWrite(RLED, HIGH);
        delay(500);
        writeConfiguration();

      }
      else if (numberPulseCount == 9){
                
        autoRestart = !autoRestart;
        String msg = "  - Auto-restart changed to: " + String(autoRestart) + " (1: Enabled, 0: Disabled)";
        Serial.println(msg);
        log("Admin",msg);
        if (autoRestart)
          digitalWrite(GLED, HIGH);
        else
          digitalWrite(RLED, HIGH);
        delay(500);
        writeConfiguration();
        
      }
      else if (numberPulseCount == 10){
        digitalWrite(GLED, LOW);
        state = AdminIdle;
        Serial.println("Back to Admin Idle");
        delay(500);
        digitalWrite(GLED, HIGH);
      }
      else{
        digitalWrite(RLED, LOW);
        Serial.println("(Command not supported)");
        delay(500);
        digitalWrite(RLED, HIGH);
      }
      numberPulseCount = 0;
    }

    if (modeSwitch.fell()){
        if(digitalRead(SWH) == LOW){
           Serial.println("Exit Command and wait ");
           state = Wait; 
        }
        else{
          Serial.println("Exit Command and stall");
          state = Stalling;
        }
      }


    break; // from Command
    
   }

   case PlayNext:
   {
      digitalWrite(GLED, LOW);
      digitalWrite(RLED, HIGH);

      if (numberSwitch.rose()){
        numberPulseCount++;
      }

      float vol = analogRead(VOL);
      vol = vol / 1024;
      audioBoard.volume(vol);

      if(dialSwitch.rose()){
        if (selectedListSize > 0){

              // next
              if (numberPulseCount == 1){
                playNextIndex++;    
                if (playNextIndex >= selectedListSize) {
                  Serial.println("  List rollover...");
                  playNextIndex = 0;
                }
                Serial.println("  - Next");
                playing = true;
                if (interviewsSelected)
                  playInterview(selectedList[playNextIndex]);
                else
                  playRecording(selectedList[playNextIndex]); 
               }
      
               // previous
               else if (numberPulseCount == 2){
                playNextIndex--;    
                if (playNextIndex < 0) {
                  Serial.println("  - List rollover...");
                  playNextIndex = selectedListSize-1;
                }
                Serial.println("  - Previous");
                playing = true;
                if (interviewsSelected)
                  playInterview(selectedList[playNextIndex]);
                else
                  playRecording(selectedList[playNextIndex]); 
               }
               
               numberPulseCount = 0;
        }
        else {
          Serial.println("List is empty");
          }
       } 

      if(!playWav1.isPlaying() && playing == true){
        Serial.println("Finished");
        playing = false;
      }
      if(hookSwitch.rose()){
        Serial.println("Hook down, back to Admin idle");
        playing = false;
        state = AdminIdle;  
      }

      if (modeSwitch.fell()){
        Serial.println("Exit Admin and wait hook down");
        playing = false;
        state = Wait;  
      }

   
      break; // from PlayNext
   }
   
  
  } // end switch
    
  if(hookSwitch.rose() && digitalRead(SWM) == LOW && state != Idle){
    Serial.println("Hook down");
    if (state == Recording)
      stopRecording();
    
    if (state == Playing){
      float totalDuration  = playWav1.lengthMillis();
      float current = playWav1.positionMillis();
      playWav1.stop();
      float retentionRate =  current/totalDuration*100.0;
      String msg = "Interview '" + currentInterview + "' abandoned at " + String(retentionRate) + "% (@ " + String(current/1000) + "/" + String(totalDuration/1000) + " seconds)";
      Serial.println(msg);
      log("Playing abandoned", msg);
      
    }
    
    state = Stalling;  
  
  }

  //if (modeSwitch.rose()){
  //  Serial.println("Admin Idle mode");
  //  if (state == Recording)
  //    stopRecording();
  //  state = AdminIdle;  
  //}

}


//==================================================== Utility functions =================================================== //

void logSettings(){

String msg = "preRingInterval: " + String(preRingInterval/1000) + "; stallPeriod: " + String(stallPeriod/1000) + "; recInterval: " + String(recInterval/1000) + "; recEnabled: " + String(recEnabled) + "; ringerEnabled: " + String(ringerEnabled) + "; interviewsSelected: " + String(interviewsSelected) + "; cityIndex: " + String(cityIndex) + "; prePlayPrompt: " + String(prePlayPrompt) + "; autoRestart: " + String(autoRestart);

log("Settings", msg);

}


void restart(){
  Serial.println("Restarting in 3 seconds...");
  digitalWrite(RLED, HIGH);
  delay(1000);
  digitalWrite(RLED, LOW);
  delay(1000);
  digitalWrite(RLED, HIGH);
  delay(1000);
  digitalWrite(RLED, LOW);
  WRITE_RESTART(0x5FA0004);  
}


boolean canRecord(){
  
  if(recEnabled)
  {
    if(hasEnoughSpaceForMb(recNeededSize)){
      boolean hasSlot = false;
      for(int i=0;i<RECODINGSMAXSIZE;i++){
          if (recordingslist[i] == "_"){
            hasSlot = true;
            break;
           }
       }
       
       if(!hasSlot){
        Serial.print("  - SD has space to record, but all (");
        Serial.print(RECODINGSMAXSIZE);
        Serial.println(") recording slots are filled");
        log("WARN","Cant record. Recording slots are filled");
       }
       
       return hasSlot;
    }
    else{
      Serial.println("  - No enough space for recording");
      return false;
    }
  }
  else {
    Serial.println("  - Recording is diabled");
    return false;
  }
}

void playInterview(String filename)
{
  playWav1.stop();
  Serial.print("Playing Interview: ");
  Serial.println(filename);

  // Start playing the file.  This sketch continues to
  // run while the file plays.
  String file = city[cityIndex] + "/Interviews/" + filename;
  char buf[file.length() + 1];
  file.toCharArray(buf, file.length() + 1);
  
  playWav1.play(buf);

  // A brief delay for the library read WAV info
  delay(25);

}

void playMessage(String filename)
{
  playWav1.stop();
  Serial.print("Playing msg: ");
  Serial.println(filename);

  // Start playing the file.  This sketch continues to
  // run while the file plays.
  String file = city[cityIndex] + "/Messages/" + filename;
  char buf[file.length() + 1];
  file.toCharArray(buf, file.length() + 1);
  
  playWav1.play(buf);

  // A brief delay for the library read WAV info
  delay(25);

}

void playRecording(String filename)
{
  playWav1.stop();
  Serial.print("Playing Recording: ");
  Serial.println(filename);

  // Start playing the file.  This sketch continues to
  // run while the file plays.
  String file = city[cityIndex] + "/Recordings/" + filename;
  char buf[file.length() + 1];
  file.toCharArray(buf, file.length() + 1);
  
  playWav1.play(buf);

  // A brief delay for the library read WAV info
  delay(25);

}


void stop(){
  Serial.println("STOPPED");
  log("ERROR","Stopped");
  while(true) {}
  }
  
void checkSd(){
  Sd2Card card;
  SdVolume volume;
  uint64_t size, freeSize;
  int type;
  
  Serial.print(F("- SD Card: "));
  
  // First, detect the card
  status = card.init(SPI_FULL_SPEED, SDCARD_CS_PIN);
  if (status) {
    Serial.print(F("Connected"));
  } else {
    Serial.println(F("ERROR: SD card is not connected or unusable"));
    stop();
  }

  type = card.type();
  if (type == SD_CARD_TYPE_SD1 || type == SD_CARD_TYPE_SD2) {
    Serial.print(F(", SD"));
  } else if (type == SD_CARD_TYPE_SDHC) {
    Serial.print(F(", SDHC"));
  } else {
    Serial.print(F(", unknown type (maybe SDXC?)"));
  }

  // Then look at the file system and print its capacity
  status = volume.init(card);
  if (!status) {
    Serial.println(F("ERROR: Unable to access the filesystem on this card"));
    stop();
  }


  // print the type and size of the first FAT-type volume

  Serial.print(", FAT");
  Serial.print(volume.fatType(), DEC);

  freeSize = volume.blocksPerCluster() * SD.sdfs.freeClusterCount();
  freeSize = freeSize * (512.0 / 1e6); // convert blocks to millions of bytes
  Serial.print(", ");
  Serial.print(freeSize);
  Serial.print(F(" MB Free"));

  size = volume.blocksPerCluster() * volume.clusterCount();
  size = size * (512.0 / 1e6); // convert blocks to millions of bytes
  Serial.print(" / ");
  Serial.print(size);
  Serial.println(F(" MB Total"));

}

boolean hasEnoughSpaceForMb(unsigned int needed){

  // needed: size in MB
  // 10 could be a good value (since 2 mins of  16 bit, mono @ 44,1Khz approximate to 10MB)
 
 SdVolume volume;
 uint64_t freeSize;
 freeSize = volume.blocksPerCluster() * SD.sdfs.freeClusterCount();
 freeSize = freeSize * (512.0 / 1e6); // convert blocks to millions of bytes
 Serial.print(F("- SD free space MB: "));
 Serial.print(freeSize);
 Serial.print(F(", Needed MB: "));
 Serial.print(needed);
 
 if (freeSize > needed) {
  Serial.println(". OK");
  return true;
 }
 else {
  Serial.println(". NOK");
  return false;
 }
 

}

int fillPlayList() {
  
  // prints to serial files found
  // fills the array playlist
  // returns number of files found in folder /Interviews
    
  int cnt = 0;
  for(int i=0; i<PLAYLISTMAXSIZE; i++) {
    playlist[i] = "";
  }
  
  String path = city[cityIndex] + "/Interviews";
  Serial.print(F("- Playlist: "));
  Serial.println(path);
  
  char buf[path.length() + 1];
  path.toCharArray(buf, path.length() + 1);
  
  File dir = SD.open(buf);
   while(true) {
     File entry = dir.openNextFile();
     if (! entry) {
       //no more files
       break;
     }
     
     if (entry.isDirectory()) {
      // skip directories
       continue;
     } else {
            String filename = entry.name();
            if (filename.startsWith("INT_") && filename.endsWith(".wav")) {
              Serial.print(F("  - "));
              Serial.println(filename);
              playlist[cnt] = filename;
              cnt++;
            }       
     }
     entry.close();
   }
   playlistSize = cnt;
   return cnt;
}

String pickWavFile(){
  int index;
  //randomSeed(analogRead(RAND));
  index = random(0,playlistSize);
  return playlist[index];
  }

time_t getTeensyTime()
{
  return Teensy3Clock.get();
}

String padDigits(int digits){
  String tmp = "";
  if(digits < 10)
    tmp = "0" + String(digits);
  else
    tmp = String(digits);
  return tmp;
}

void log(String data1, String data2){
  // logs data1 and data2 as comma separated log line
  // adds a datetime
  
  String datetime = getDateTime();

  File dataFile = SD.open("history.log", FILE_WRITE);

  // if the file is available, write to it:
  if (dataFile) {
    dataFile.print(datetime);
    dataFile.print(",");
    dataFile.print(city[cityIndex]);
    dataFile.print(",");
    dataFile.print(data1);
    dataFile.print(",");
    dataFile.println(data2);
    dataFile.close();
  } else {
    Serial.println("ERROR: can't open history.log");
  } 
}

String getDateTime(){
  time_t t = now();
  String datetime = String(year(t)) + "-" + String(padDigits(month(t))) + "-" + String(padDigits(day(t))) + " " + String(hour(t)) + ":" + String(padDigits(minute(t))) + ":" + String(padDigits(second(t)));
  return datetime;

}

int getCityArrayLen() {
   return (int)sizeof(city)/sizeof(city[0]);
}
//==================================================== EEPROM functions =================================================== //

void writeConfiguration(){
    EEPROM.update(1,recEnabled);
    EEPROM.update(2,ringerEnabled);
    //stall period (int)
    EEPROM.update(3, stallPeriod >> 8);
    EEPROM.update(4, stallPeriod & 0xFF);
    EEPROM.update(9,interviewsSelected);
    EEPROM.update(10,cityIndex);
    EEPROM.update(11,prePlayPrompt);
    EEPROM.update(12,autoRestart);
    EEPROM.update(13, preRingInterval >> 8);
    EEPROM.update(14, preRingInterval & 0xFF);
    
    EEPROM.update(ADDR_EEPROM_MARKER, MARKER_EEPROM);
    
}

void initializeEEPROM()
{
  //copy vars from EEPROM if available, else, write to EEPROM from code
  
  byte marker = EEPROM.read(ADDR_EEPROM_MARKER);
  if (marker == MARKER_EEPROM)
  {
    Serial.print("- Reading config from EEPROM >");
    recEnabled = EEPROM.read(1);
    ringerEnabled = EEPROM.read(2);
    stallPeriod = (EEPROM.read(3) << 8) + EEPROM.read(3 + 1);
    recInterval = readLongFromEEPROM(5);
    interviewsSelected = EEPROM.read(9);
    cityIndex = EEPROM.read(10);
    prePlayPrompt = EEPROM.read(11);
    autoRestart = EEPROM.read(12);
    preRingInterval = (EEPROM.read(13) << 8) + EEPROM.read(13 + 1);
    // if you want more vars later on, start at address 15
    
    Serial.print(" recEnabled: ");
    Serial.print(recEnabled);
    Serial.print(", ringerEnabled: ");
    Serial.print(ringerEnabled);
    Serial.print(", stallPeriod: ");
    Serial.print(stallPeriod);
    Serial.print(", recInterval: ");
    Serial.print(recInterval);
    Serial.print(", interviewsSelected: ");
    Serial.print(interviewsSelected);
    Serial.print(", cityIndex: ");
    Serial.print(cityIndex);
    Serial.print(", prePlayPrompt: ");
    Serial.print(prePlayPrompt);
    Serial.print(", autoRestart: ");
    Serial.print(autoRestart);
    Serial.print(", preRingInterval: ");
    Serial.println(preRingInterval);
    
  }
  else {
    
    Serial.print("- EEPROM not initialized. Setting config from code instead >");
    EEPROM.update(1,recEnabled);
    EEPROM.update(2,ringerEnabled);
    //stall period (uint)
    EEPROM.update(3, stallPeriod >> 8);
    EEPROM.update(4, stallPeriod & 0xFF);
    writeLongIntoEEPROM(5, recInterval);
    EEPROM.update(9,interviewsSelected);
    EEPROM.update(10,cityIndex);
    EEPROM.update(11,prePlayPrompt);
    EEPROM.update(12,autoRestart);
    //preRingInterval (uint)
    EEPROM.update(13, preRingInterval >> 8);
    EEPROM.update(14, preRingInterval & 0xFF);
    // if you want more vars later on, start at address 15
    
    Serial.print(" recEnabled: ");
    Serial.print(recEnabled);
    Serial.print(", ringerEnabled: ");
    Serial.print(ringerEnabled);
    Serial.print(", stallPeriod: ");
    Serial.print(stallPeriod);
    Serial.print(", recInterval: ");
    Serial.print(recInterval);
    Serial.print(", interviewsSelected: ");
    Serial.print(interviewsSelected);
    Serial.print(", cityIndex: ");
    Serial.print(cityIndex);
    Serial.print(", prePlayPrompt: ");
    Serial.print(prePlayPrompt);
    Serial.print(", autoRestart: ");
    Serial.print(autoRestart);
    Serial.print(", preRingInterval: ");
    Serial.println(preRingInterval);
    
    // save marker
    EEPROM.update(ADDR_EEPROM_MARKER, MARKER_EEPROM);
  }
}

void writeLongIntoEEPROM(int address, long number)
{ 
  EEPROM.write(address, (number >> 24) & 0xFF);
  EEPROM.write(address + 1, (number >> 16) & 0xFF);
  EEPROM.write(address + 2, (number >> 8) & 0xFF);
  EEPROM.write(address + 3, number & 0xFF);
}
long readLongFromEEPROM(int address)
{
  return ((long)EEPROM.read(address) << 24) +
         ((long)EEPROM.read(address + 1) << 16) +
         ((long)EEPROM.read(address + 2) << 8) +
         (long)EEPROM.read(address + 3);
}

//==================================================== Recording functions =================================================== //

void startRecording() {
  
  String filename = "none";
  // add new file to list
  for(int i=0;i<RECODINGSMAXSIZE;i++){
    if (recordingslist[i] == "_"){
      filename = String(i+1) + ".wav";
      recordingslist[i] = filename;
      recordingsListSize = i+1;
      break;
    }
  }


  String path = city[cityIndex] + "/Recordings/" + filename;  

  log("Recording", filename);
  
  char buf[path.length() + 1];
  path.toCharArray(buf, path.length() + 1);
  
  Serial.print("Recording Started: ");
  Serial.print(path);
  Serial.print(", Max duration(sec): ");
  Serial.println(recInterval/1000);
  
  if (SD.exists(buf)) {
    SD.remove(buf);
  }
  frec = SD.open(buf, FILE_WRITE);
  if (frec) {
    queue1.begin();
    recByteSaved = 0L;
  }
  
}

void continueRecording() {
  
  if (queue1.available() >= 2) {
    byte buffer[512];
    memcpy(buffer, queue1.readBuffer(), 256);
    queue1.freeBuffer();
    memcpy(buffer + 256, queue1.readBuffer(), 256);
    queue1.freeBuffer();
    // write all 512 bytes to the SD card
    frec.write(buffer, 512);
    recByteSaved += 512;
  }
}

void stopRecording() {
  Serial.println("- Stopping Recording...");
  queue1.end();
  float recDuration = (millis() - previousRecMillis)/1000;

  while (queue1.available() > 0) {
    frec.write((byte*)queue1.readBuffer(), 256);
    queue1.freeBuffer();
    recByteSaved += 256;
  }
  writeOutHeader();
  frec.close();
  
  String msg = "- Recording stopped (duration: " + String(recDuration) + " sec)";
  log("Recording stopped", msg);
  Serial.println(msg);
  
}

int fillRecordingsList() {
   
  int cnt = 0;
  
  for(int i=0; i<RECODINGSMAXSIZE; i++) {
    recordingslist[i] = "_";
  }
  
  String path = city[cityIndex] + "/Recordings";
  Serial.print(F("- Recordings list: "));
  Serial.println(path);
   
  char buf[path.length() + 1];
  path.toCharArray(buf, path.length() + 1);
  
  File dir = SD.open(buf);
   while(true) {
     File entry = dir.openNextFile();
     if (! entry) {
       //no more files
       break;
     }
     
     if (entry.isDirectory()) {
      // skip directories
       continue;
     } else {
            String filename = entry.name();
            if (filename.endsWith(".wav")) {
              Serial.print(F("  - "));
              Serial.println(filename);
              recordingslist[cnt] = filename;
              cnt++;
            }       
     }
     entry.close();
   }

   recordingsListSize = cnt;
   return cnt;
}


void writeOutHeader() { // update WAV header with final filesize/datasize

//  NumSamples = (recByteSaved*8)/bitsPerSample/numChannels;
//  Subchunk2Size = NumSamples*numChannels*bitsPerSample/8; // number of samples x number of channels x number of bytes per sample
  Subchunk2Size = recByteSaved;
  ChunkSize = Subchunk2Size + 36;
  frec.seek(0);
  frec.write("RIFF");
  byte1 = ChunkSize & 0xff;
  byte2 = (ChunkSize >> 8) & 0xff;
  byte3 = (ChunkSize >> 16) & 0xff;
  byte4 = (ChunkSize >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  frec.write("WAVE");
  frec.write("fmt ");
  byte1 = Subchunk1Size & 0xff;
  byte2 = (Subchunk1Size >> 8) & 0xff;
  byte3 = (Subchunk1Size >> 16) & 0xff;
  byte4 = (Subchunk1Size >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  byte1 = AudioFormat & 0xff;
  byte2 = (AudioFormat >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  byte1 = numChannels & 0xff;
  byte2 = (numChannels >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  byte1 = sampleRate & 0xff;
  byte2 = (sampleRate >> 8) & 0xff;
  byte3 = (sampleRate >> 16) & 0xff;
  byte4 = (sampleRate >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  byte1 = byteRate & 0xff;
  byte2 = (byteRate >> 8) & 0xff;
  byte3 = (byteRate >> 16) & 0xff;
  byte4 = (byteRate >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  byte1 = blockAlign & 0xff;
  byte2 = (blockAlign >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  byte1 = bitsPerSample & 0xff;
  byte2 = (bitsPerSample >> 8) & 0xff;
  frec.write(byte1);  frec.write(byte2); 
  frec.write("data");
  byte1 = Subchunk2Size & 0xff;
  byte2 = (Subchunk2Size >> 8) & 0xff;
  byte3 = (Subchunk2Size >> 16) & 0xff;
  byte4 = (Subchunk2Size >> 24) & 0xff;  
  frec.write(byte1);  frec.write(byte2);  frec.write(byte3);  frec.write(byte4);
  frec.close();
  //Serial.println("header written"); 
  Serial.print("- Saved: File size is "); 
  Serial.print(Subchunk2Size);
  Serial.println(" bytes"); 
}

int setRecNeededSize(){

  switch(recInterval){

    case 30000:
    {
      recNeededSize = 3;
      break;
    }

    case 60000:
    {
      recNeededSize = 6;
      break;
    }

    case 120000:
    {
      recNeededSize = 11;
      break;
    }

    case 180000:
    {
      recNeededSize = 16;
      break;
    }
    
  }

  return recNeededSize;

}
