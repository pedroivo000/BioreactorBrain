#include <Adafruit_LiquidCrystal.h>
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include <Ethernet.h>
///////////////////////////////// VARIABLE DECLARATION AND LIBRARY SETUP ////////////////////////////////////////////
//Initialize the LCD library with I2C:
Adafruit_LiquidCrystal lcd(0);

//Initialize pins:
const int tempProbe = A0;
byte padRelayPin = 22;
byte motorPin = 8;

//Log file:
File logFile;
char fileName[] = "BB1LOG00.CSV";
char requestedFile[30];

//Atlas Scientific I2C probes setup:
#define TOTAL_CIRCUITS 2    //set how many I2C circuits are connected
int probe_ids[] = {97, 99}; //I2C addresses of the connected probes (see datasheets for the probes to get the values)
char *probe_names[] = {"DO", "pH"};   //Probe names
char sensordata[30];                  // A 30 byte character array to hold incoming data from the sensors
String sensorData[TOTAL_CIRCUITS];    //Array that will hold the sensor data for printing to LCD screen and reactor control
byte sensor_bytes_received = 0;       // We need to know how many characters bytes have been received
byte code = 0;                        // used to hold the I2C response code.
byte in_char = 0;                     // used as a 1 byte buffer to store in bound bytes from the I2C Circuit.

//Initialize webserver: 
// Enter a MAC address for your controller below.
byte mac[] = {0x00, 0xAA, 0xBB, 0xCC, 0xDE, 0x02};
EthernetServer server = EthernetServer(80);

//Time keeping:
unsigned long previousMillis = 0;
const long measureInterval = 5000;

void setup() {
  pinMode(padRelayPin, OUTPUT);
  pinMode(motorPin, OUTPUT);

  Serial.begin(9600);
  Wire.begin(); //Initialize I2C bus as slave
  
  //set up the LCD:
  lcd.begin(20, 4);
  lcd.print("BioreactorBrain v2.0");
  //Showing the user if everything is going ok during initialization:
  lcd.setCursor(0,1);
  lcd.print("1. SD card...");
  
  //Initializing SD card:
  pinMode(10, OUTPUT);
  digitalWrite(10, HIGH); //turning ethernet off for just a little bit
  
  //1.Check if card is present and can be used:
  if(!SD.begin(4)){
    lcd.setCursor(10,1);
    lcd.print(": X");
    return; //do nothing else
  }else{
    lcd.setCursor(10,1);
    lcd.print(": OK");
  }

  //2.Creating log file: 
  for (uint8_t i = 0; i < 100; i++) {
    fileName[6] = i/10 + '0';
    fileName[7] = i%10 + '0';
    if (SD.exists(fileName)) {
      continue;
    }
    logFile = SD.open(fileName, FILE_WRITE);
    break;
  }
  if(!logFile){
    lcd.setCursor(0,2);
    lcd.print("Log file error!");
  }else{
    lcd.setCursor(0,2);
    lcd.print("Log = ");
    lcd.print(logFile.name());  
  }

  logFile.close(); //We need to close the file here so we can log the data and 
                   //use the webserver capabilities later in the code. 

  //Initializing ethernet connection:
  lcd.setCursor(0,3);
  lcd.print("2. Internet..."); 
  if(Ethernet.begin(mac) == 0) {
    lcd.setCursor(11,3);
    lcd.print(": X");
  } else{
    lcd.setCursor(11,3);
    lcd.print(": OK");
    server.begin();
  }
  delay(5000);
  lcd.clear();
  lcd.print("Sensor readings:");
}

void loop() {
  float temp; //temperature of reactor
  String dataString = ""; //string for data logging
  unsigned long currentMillis = millis(); //current time since beggining of run
  
  /////////////////////////// DATA COLLECTION /////////////////////////////////
  if(currentMillis - previousMillis >= measureInterval) {
    previousMillis = currentMillis;
    //Getting temperature from sensor:
    temp = getTemp(tempProbe);
    //Using this temperature to correct the readings from the DO and ph probes:
    I2CtempCompensation(temp);
  
    //Append temp to data logging string:
    dataString = String(currentMillis/1000) + "," + String(temp);
    //I2C data collection:
    dataString += getI2Cdata();
    
    //Writing to log file: 
//    Serial.println(dataString);
    logFile = SD.open(fileName, FILE_WRITE);
    if(logFile){
      logFile.println(dataString);
      logFile.close();
    }
  
    //Print to LCD screen 
    lcd.setCursor(0, 1);
    //Printing the DO and pH probe data:
    for (byte i = 0; i<TOTAL_CIRCUITS; i++){
      lcd.print(probe_names[i]);
      lcd.print(" ");
      lcd.print(sensorData[i]);
      lcd.print("  ");
    }
    //Printing temperature:
    lcd.setCursor(0,2);
    lcd.print("T ");
    lcd.print(temp);

    //////////////////////////// REACTOR CONTROL ////////////////////////////////
    //Heating pad control:
    if(temp < 37){
        digitalWrite(padRelayPin, LOW); //turn pad to heat water
    }else if (temp >37) {
        digitalWrite(padRelayPin, HIGH); //shut pad off when desired temperature is reached
    }

    //Motor control:
//    //First: testing if we can control speed using the serial input:
//    while(Serial.available()>0){
//      int motorSpeed = Serial.parseInt();
//      motorSpeed = constrain(motorSpeed, 0,255);
//
//      Serial.println(motorSpeed);
//      analogWrite(motorPin, motorSpeed);
//    }
    analogWrite(motorPin, 255); //full speed = 200rpm
  } 
  else {
    ////////////////////////////// WEB SERVER //////////////////////////////////
    //Webserver stuff (modified from: http://www.ladyada.net/learn/arduino/ethfiles.html)
    char clientline[100];
    int index = 0;
    
    //listen for incoming clients:
    EthernetClient client = server.available();
    if (client) {
      //First, we need to close the logfile: 
      
      // an http request ends with a blank line
      boolean current_line_is_blank = true;
    
      // reset the input buffer
      index = 0;
    
      while (client.connected()) {
        if (client.available()) {
          char c = client.read();
          // If it isn't a new line, add the character to the buffer
          if (c != '\n' && c != '\r') {
            clientline[index] = c;
            index++;
            continue;
          }
          // got a \n or \r new line, which means the string is done
          clientline[index] = 0;
          // Print it out for debugging
          Serial.println(clientline);
          // Look for substring such as a request to get the root file
          if (strstr (clientline, "GET / ") != 0) {
            // print all the files:
            client.println("Files:");
            ListFiles(client);
          } 
          else if (strstr (clientline, "GET /") != 0) {
            // this time no space after the /, so a sub-file!
            char *filename;
            filename = clientline + 5; // look after the "GET /" (5 chars)
            // a little trick, look for the " HTTP/1.1" string and 
            // turn the first character of the substring into a 0 to clear it out.
            (strstr (clientline, " HTTP"))[0] = 0;
            // print the file we want
            Serial.println(filename);
    
            //Opening file: 
            File file = SD.open(filename);
            if (file) {
              Serial.println("Opened!");
              int16_t c;
              while ((c = file.read()) > 0) {
                // uncomment the serial to debug (slow!)
                Serial.print((char)c);
                client.print((char)c);
            }
            file.close();
            }else{
              Serial.println("File not found!");  
              client.println("File Not Found!");
            }
          }
          break;
        }
      }
      // give the web browser time to receive the data
      delay(1);
      client.stop();
    }
  }
}

///////////////////////////////// FUNCTIONS ///////////////////////////////////
String getI2Cdata() {
  String I2Cdata = "";
  
  for (int i = 0; i < TOTAL_CIRCUITS; i++) {       // loop through all the sensors
    Wire.beginTransmission(probe_ids[i]);     // call the circuit by its ID number.
    Wire.write('r');                          // request a reading by sending 'r'
    Wire.endTransmission();                         // end the I2C data transmission.
    
    delay(1000);  // AS circuits need a second before the reading is ready

    sensor_bytes_received = 0;                        // reset data counter
    memset(sensordata, 0, sizeof(sensordata));        // clear sensordata array;

    Wire.requestFrom(probe_ids[i], 48, 1);    // call the circuit and request 48 bytes (this is more then we need).
    code = Wire.read();

    while (Wire.available()) {          // are there bytes to receive?
      in_char = Wire.read();            // receive a byte.

      if (in_char == 0) {               // null character indicates end of command
        Wire.endTransmission();         // end the I2C data transmission.
        break;                          // exit the while loop, we're done here
      }
      else {
        sensordata[sensor_bytes_received] = in_char;      // append this byte to the sensor data array.
        sensor_bytes_received++;
      }
    }

    switch (code) {                       // switch case based on what the response code is.
      case 1:                             // decimal 1  means the command was successful.
        sensorData[i] = sensordata;
        
        I2Cdata += "," + String(sensordata);
        break;                              // exits the switch case.

      case 2:                             // decimal 2 means the command has failed.
        sensorData[i] = "command failed";
        I2Cdata += ",command failed";
        break;                              // exits the switch case.

      case 254:                           // decimal 254  means the command has not yet been finished calculating.
        sensorData[i] = "circuit not ready";
        I2Cdata += ",circuit not ready";
        break;                              // exits the switch case.

      case 255:                           // decimal 255 means there is no further data to send.
        sensorData[i] = "no data";
        I2Cdata += ",no data";
        break;                              // exits the switch case.
    }    
  } // for loop

  return(I2Cdata);
}

float getTemp(int probePin) {
  int voltage = analogRead(probePin);
  long Resistance; 
  float Resistor = 15000; //bridge resistor
  // the measured resistance of your particular bridge resistor in
  //the Vernier BTA-ELV this is a precision 15K resisitor 
  float Temp;  // Dual-Purpose variable to save space.
  Resistance=( Resistor*voltage /(1024-voltage)); 
  Temp = log(Resistance); // Saving the Log(resistance) so not to calculate  it 4 times later
  Temp = 1 / (0.00102119 + (0.000222468 * Temp) + (0.000000133342 * Temp * Temp * Temp));
  Temp = Temp - 273.15;  // Convert Kelvin to Celsius                      
return Temp;                                      // Return the Temperature
}

void I2CtempCompensation(float temp) {
  String request = "t," + String(temp); //create temperature compensation command 
  for (byte i = 0; i < TOTAL_CIRCUITS ; i++) {       // loop through all the sensors
    Wire.beginTransmission(probe_ids[i]);     // call the circuit by its ID number.
    Wire.write('request');                          // send temperature value to correct readings
    Wire.endTransmission();                         // end the I2C data transmission.
    delay(300);
  }
}

void ListFiles(EthernetClient client) {
  File root = SD.open("/");
  root.rewindDirectory();
  while (true) {
    File entry =  root.openNextFile();
    if (!entry) {
      // no more files
      break;
    }
    if (entry.isDirectory()) {
      continue;
    } else {
      // files have sizes, directories do not
      client.print(entry.name());
      client.print("\t");
      client.println(entry.size(), DEC);
    }
    entry.close();
  }
}

