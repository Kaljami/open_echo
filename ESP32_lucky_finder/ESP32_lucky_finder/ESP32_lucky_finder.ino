
/*
  ESP32-C3 SuperMini
  Interrupt-triggered analog sampling
*/

const int interruptPin = 1;  // GPIO 2: interrupt input
const int analogPin = 4;      // GPIO 0: analog input (ADC)
const int numSamples = 250;

volatile bool interruptFlag = false;

int analogValues[numSamples];

// Declare ISR before setup()
void IRAM_ATTR interruptServiceRoutine();

void setup() {
  Serial.begin(115200);
  sleep(2);
  Serial.print("Serial ready");

  pinMode(interruptPin, INPUT);
  pinMode(analogPin, INPUT);

  analogReadResolution(12);  // 0-4095

  attachInterrupt(
    digitalPinToInterrupt(interruptPin),
    interruptServiceRoutine,
    FALLING
  );
}

void loop() {
  if (interruptFlag) {
    // Clear the flag safely
    noInterrupts();
    interruptFlag = false;
    interrupts();

    // Read 300 analog samples
    for (int i = 0; i < numSamples; i++) {
      analogValues[i] = analogRead(analogPin);
    }

    // Print samples in the original format
    Serial.print("sp");

    for (int i = 0; i < numSamples; i++) {
      Serial.print(analogValues[i]);

      if (i < numSamples - 1) {
        Serial.print(", ");
      }
    }

    Serial.println();
  }
}

// Interrupt Service Routine
void IRAM_ATTR interruptServiceRoutine() {
  interruptFlag = true;
}
