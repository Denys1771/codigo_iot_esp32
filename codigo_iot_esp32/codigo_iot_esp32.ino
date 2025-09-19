#include <ThingerESP32.h>
#include <RTClib.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Preferences.h>

// Credenciales de Thinger.io
// Se cambiaron por los valores 'x' para evitar
// el acceso de terceros al control del dispositivo 
#define USERNAME "xxxxx"
#define DEVICE_ID "xxxxxxx
#define DEVICE_CREDENTIAL "xxxxxxxxx"

// Credenciales de WiFi
#define WIFI_SSID "RXDS"
#define WIFI_PASSWORD "418936181717"

// Pines
#define PIN_BOMBA 27
#define PIN_BOYA 25   // Boya: abierto = hay agua, cerrado = no hay agua

// Objetos para DS3231, NTP y Preferences
RTC_DS3231 rtc;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -18000); // UTC-5
Preferences prefs;

// Variables de programación
bool isModoAutomatico = false;
int horaEncendido = 18;
int minutoEncendido = 0;
bool diasSemana[7] = {false, false, false, false, false, false, false};
bool bombaEstado = false;
bool ultimoEstado = false;
bool activacionAutomaticaEnCurso = false;
long lastTriggerMinuteEpoch = -1;
unsigned long lastModoUpdate = 0;
unsigned long lastBombaUpdate = 0;
unsigned long lastHoraUpdate = 0;
unsigned long lastMinutoUpdate = 0;

// --- Variables para historial de bomba ---
unsigned long bombaEncendidaTimestamp = 0; // Momento en que la bomba se enciende
bool ultimoHayAgua = true;

bool modoInicializado = false;
ThingerESP32 thing(USERNAME, DEVICE_ID, DEVICE_CREDENTIAL);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Iniciando ESP32...");

  pinMode(PIN_BOMBA, OUTPUT);
  digitalWrite(PIN_BOMBA, HIGH);
  bombaEstado = false;
  Serial.println("Bomba inicializada: APAGADA");

  pinMode(PIN_BOYA, INPUT_PULLUP); // Boya como entrada con resistencia interna

  if (!rtc.begin()) {
    Serial.println("No se encontró el DS3231");
    while (1);
  }

  // Conectar WiFi
  Serial.print("Conectando a WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConectado a WiFi");
    Serial.print("Dirección IP: ");
    Serial.println(WiFi.localIP());
    thing.add_wifi(WIFI_SSID, WIFI_PASSWORD);
  } else {
    Serial.println("\nFallo al conectar a WiFi, usando DS3231 como respaldo");
  }

  timeClient.begin();
  syncTimeWithNTP();

  // ---- Cargar valores guardados ----
  prefs.begin("riego", false);
  horaEncendido = prefs.getInt("hora", horaEncendido);
  minutoEncendido = prefs.getInt("minuto", minutoEncendido);
  isModoAutomatico = prefs.getBool("modo", isModoAutomatico);
  diasSemana[0] = prefs.getBool("dia0", false);
  diasSemana[1] = prefs.getBool("dia1", false);
  diasSemana[2] = prefs.getBool("dia2", false);
  diasSemana[3] = prefs.getBool("dia3", false);
  diasSemana[4] = prefs.getBool("dia4", false);
  diasSemana[5] = prefs.getBool("dia5", false);
  diasSemana[6] = prefs.getBool("dia6", false);
  prefs.end();

  // Mostrar modo guardado
  if (isModoAutomatico) {
    Serial.printf("Modo guardado: AUTOMÁTICO -> Hora: %02d Minuto: %02d\n", horaEncendido, minutoEncendido);
  } else {
    Serial.println("Modo guardado: MANUAL");
  }

  // ---- Recursos Thinger.io ----

  // Control manual de bomba
  thing["bomba"] << [](pson &in) {
    if (!isModoAutomatico) {
      bool nuevoEstado = in ? true : false;
      if (millis() - lastBombaUpdate > 500 && nuevoEstado != bombaEstado) {
        bombaEstado = nuevoEstado;
        activacionAutomaticaEnCurso = false;
        digitalWrite(PIN_BOMBA, bombaEstado ? LOW : HIGH);
        Serial.println(bombaEstado ? "Bomba encendida (manual)" : "Bomba apagada (manual)");
        thing.stream(thing["estado_bomba"]);
        lastBombaUpdate = millis();
      }
    } else {
      Serial.println("Ignorando comando de bomba: modo automático activo");
    }
  };

  // Estado de bomba (solo salida)
  thing["estado_bomba"] >> [](pson &out) {
    out = bombaEstado ? "Encendida" : "Apagada";
  };

  // --- Estado de la boya en dashboard ---
  thing["estado_balde"] >> [](pson &out) {
    bool hayAgua = digitalRead(PIN_BOYA);
    out = hayAgua ? "Lleno" : "Vacío";
  };

  // --- Recurso para enviar datos al Data Bucket (solo en apagado) ---
  thing["historial_bomba"] >> [](pson &out) {
    DateTime now = rtc.now();
    out["estado"] = bombaEstado ? "Encendida" : "Apagada";
    out["duracion"] = bombaEncendidaTimestamp > 0 ? (millis() - bombaEncendidaTimestamp) / 1000 : 0;
    out["timestamp"] = now.unixtime();
    out["dia_semana"] = now.dayOfTheWeek();
  };

  // Modo automático / manual
  thing["modo"] << [](pson &in) {
    static bool primerValorRecibido = false;
    if (!primerValorRecibido) {
      in = isModoAutomatico;
      primerValorRecibido = true;
      return;
    }

    bool nuevoModo = in ? true : false;
    if (millis() - lastModoUpdate > 500 && nuevoModo != isModoAutomatico) {
      isModoAutomatico = nuevoModo;
      prefs.begin("riego", false);
      prefs.putBool("modo", isModoAutomatico);
      prefs.end();
      Serial.println(isModoAutomatico ? "Modo Automático" : "Modo Manual");

      if (!isModoAutomatico) {
        bombaEstado = false;
        activacionAutomaticaEnCurso = false;
        digitalWrite(PIN_BOMBA, HIGH);
        thing.stream(thing["estado_bomba"]);
        // --- Enviar datos al Data Bucket al apagar en modo manual ---
        if (bombaEncendidaTimestamp > 0 && WiFi.status() == WL_CONNECTED) {
          thing.stream(thing["historial_bomba"]);
          bombaEncendidaTimestamp = 0;
        }
      }
      lastModoUpdate = millis();
    }
  };

  // Inicializar flag y enviar valor inicial al dashboard
  modoInicializado = true;
  thing.stream(thing["modo"]);

  // Hora programada
  thing["hora_programada"] = [](pson &in, pson &out) {
    if (in.is_empty()) {
      out = horaEncendido;
    } else if (isModoAutomatico) {
      int nuevaHora = (int)in;
      if (millis() - lastHoraUpdate > 500 && nuevaHora != horaEncendido) {
        if (nuevaHora >= 0 && nuevaHora <= 23) {
          horaEncendido = nuevaHora;
          prefs.begin("riego", false);
          prefs.putInt("hora", horaEncendido);
          prefs.end();
          Serial.printf("Hora programada actualizada: %02d:%02d\n", horaEncendido, minutoEncendido);
          lastTriggerMinuteEpoch = -1;
          thing.stream(thing["hora_programada"]);
        }
        lastHoraUpdate = millis();
      }
    }
  };

  // Minuto programado
  thing["minuto_programado"] = [](pson &in, pson &out) {
    if (in.is_empty()) {
      out = minutoEncendido;
    } else if (isModoAutomatico) {
      int nuevoMinuto = (int)in;
      if (millis() - lastMinutoUpdate > 500 && nuevoMinuto != minutoEncendido) {
        if (nuevoMinuto >= 0 && nuevoMinuto <= 59) {
          minutoEncendido = nuevoMinuto;
          prefs.begin("riego", false);
          prefs.putInt("minuto", minutoEncendido);
          prefs.end();
          Serial.printf("Minuto programado actualizado: %02d:%02d\n", horaEncendido, minutoEncendido);
          lastTriggerMinuteEpoch = -1;
          thing.stream(thing["minuto_programado"]);
        }
        lastMinutoUpdate = millis();
      }
    }
  };

  // Días habilitados con persistencia
  thing["dia_0"] = [](pson &in, pson &out) {
    if (in.is_empty()) { out = diasSemana[0]; }
    else if (isModoAutomatico) {
      diasSemana[0] = (bool)in;
      prefs.begin("riego", false);
      prefs.putBool("dia0", diasSemana[0]);
      prefs.end();
      printDias();
    }
  };
  thing["dia_1"] = [](pson &in, pson &out) {
    if (in.is_empty()) { out = diasSemana[1]; }
    else if (isModoAutomatico) {
      diasSemana[1] = (bool)in;
      prefs.begin("riego", false);
      prefs.putBool("dia1", diasSemana[1]);
      prefs.end();
      printDias();
    }
  };
  thing["dia_2"] = [](pson &in, pson &out) {
    if (in.is_empty()) { out = diasSemana[2]; }
    else if (isModoAutomatico) {
      diasSemana[2] = (bool)in;
      prefs.begin("riego", false);
      prefs.putBool("dia2", diasSemana[2]);
      prefs.end();
      printDias();
    }
  };
  thing["dia_3"] = [](pson &in, pson &out) {
    if (in.is_empty()) { out = diasSemana[3]; }
    else if (isModoAutomatico) {
      diasSemana[3] = (bool)in;
      prefs.begin("riego", false);
      prefs.putBool("dia3", diasSemana[3]);
      prefs.end();
      printDias();
    }
  };
  thing["dia_4"] = [](pson &in, pson &out) {
    if (in.is_empty()) { out = diasSemana[4]; }
    else if (isModoAutomatico) {
      diasSemana[4] = (bool)in;
      prefs.begin("riego", false);
      prefs.putBool("dia4", diasSemana[4]);
      prefs.end();
      printDias();
    }
  };
  thing["dia_5"] = [](pson &in, pson &out) {
    if (in.is_empty()) { out = diasSemana[5]; }
    else if (isModoAutomatico) {
      diasSemana[5] = (bool)in;
      prefs.begin("riego", false);
      prefs.putBool("dia5", diasSemana[5]);
      prefs.end();
      printDias();
    }
  };
  thing["dia_6"] = [](pson &in, pson &out) {
    if (in.is_empty()) { out = diasSemana[6]; }
    else if (isModoAutomatico) {
      diasSemana[6] = (bool)in;
      prefs.begin("riego", false);
      prefs.putBool("dia6", diasSemana[6]);
      prefs.end();
      printDias();
    }
  };
}

void printDias() {
  Serial.print("Días habilitados: ");
  for (int i = 0; i < 7; i++) Serial.print(diasSemana[i] ? "1" : "0");
  Serial.println();
  lastTriggerMinuteEpoch = -1;
}

void syncTimeWithNTP() {
  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update();
    unsigned long epochTime = timeClient.getEpochTime();
    if (epochTime > 0) {
      rtc.adjust(DateTime(epochTime));
      Serial.println("Hora sincronizada con NTP");
    } else {
      Serial.println("Fallo en sincronización NTP");
    }
  } else {
    Serial.println("WiFi no conectado, usando DS3231");
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    thing.handle();
  } else {
    static unsigned long lastWiFiCheck = 0;
    if (millis() - lastWiFiCheck > 5000) {
      Serial.println("WiFi desconectado, intentando reconectar...");
      WiFi.reconnect();
      lastWiFiCheck = millis();
    }
  }

  static unsigned long lastSync = 0;
  if (millis() - lastSync > 3600000UL) {
    syncTimeWithNTP();
    lastSync = millis();
  }

  digitalWrite(PIN_BOMBA, bombaEstado ? LOW : HIGH);

  if (bombaEstado != ultimoEstado) {
    ultimoEstado = bombaEstado;
    Serial.println(bombaEstado ? "Bomba cambió a: ENCENDIDA" : "Bomba cambió a: APAGADA");
    thing.stream(thing["estado_bomba"]);
    // --- Registrar en Data Bucket solo al apagar en modo automático ---
    if (!bombaEstado && isModoAutomatico && bombaEncendidaTimestamp > 0 && WiFi.status() == WL_CONNECTED) {
      thing.stream(thing["historial_bomba"]);
      Serial.printf("Enviado a Data Bucket: Duración: %lu segundos\n", (millis() - bombaEncendidaTimestamp) / 1000);
      bombaEncendidaTimestamp = 0;
    }
    if (bombaEstado) {
      bombaEncendidaTimestamp = millis(); // Registrar momento de encendido
    }
  }

  // --- Enviar estado de boya al dashboard solo si cambia ---
  bool hayAgua = digitalRead(PIN_BOYA);
  if (hayAgua != ultimoHayAgua) {
    ultimoHayAgua = hayAgua;
    Serial.println(hayAgua ? "Balde con agua" : "Balde vacío");
    thing.stream(thing["estado_balde"]);
  }

  if (isModoAutomatico) {
    DateTime now = rtc.now();

    static unsigned long lastTimePrint = 0;
    if (millis() - lastTimePrint > 30000) {
      Serial.printf("Hora actual: %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());
      lastTimePrint = millis();
    }

    int horaActual = now.hour();
    int minutoActual = now.minute();
    int diaSemana = now.dayOfTheWeek();
    bool diaHabilitado = diasSemana[diaSemana];
    long minuteEpoch = (long)(now.unixtime() / 60L);

    // Encender solo si hay agua
    if (diaHabilitado &&
        horaActual == horaEncendido &&
        minutoActual == minutoEncendido &&
        minuteEpoch != lastTriggerMinuteEpoch &&
        !bombaEstado &&
        hayAgua) {
      bombaEstado = true;
      activacionAutomaticaEnCurso = true;
      Serial.printf("Bomba encendida (automático, boya OK) - Día: %d Hora: %02d:%02d\n", diaSemana, horaActual, minutoActual);
      lastTriggerMinuteEpoch = minuteEpoch;
      thing.stream(thing["estado_bomba"]);
      bombaEncendidaTimestamp = millis(); // Registrar momento de encendido
    }

    // Apagar si no hay agua
    if (!hayAgua && bombaEstado) {
      bombaEstado = false;
      activacionAutomaticaEnCurso = false;
      digitalWrite(PIN_BOMBA, HIGH);
      Serial.println("Bomba apagada: boya detectó falta de agua");
      thing.stream(thing["estado_bomba"]);
      // --- Registrar en Data Bucket al apagar por boya ---
      if (bombaEncendidaTimestamp > 0 && WiFi.status() == WL_CONNECTED) {
        thing.stream(thing["historial_bomba"]);
        Serial.printf("Enviado a Data Bucket: Duración: %lu segundos\n", (millis() - bombaEncendidaTimestamp) / 1000);
        bombaEncendidaTimestamp = 0;
      }
    }
  }
}