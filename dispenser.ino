/*
  DISPENSER DE COMIDA PARA MASCOTAS - ESP32 (Lógica 2 Fases: Base 30g + Extra hasta 50g)
  ======================================================================================

  Qué hace:
  - FASE 1 (Base mínima de 30 g):
    * Al encender (tras 3 s) o cuando el plato baja de 30 g, rellena automáticamente
      hasta alcanzar los 30 g.
    * Protección anti-susto: si el gato está comiendo (frente al sensor) o no pasaron
      30 s desde la última actividad, NO rellena hasta que el gato se retire.
  - FASE 2 (Ración extra por presencia hasta 50 g):
    * Si el plato ya tiene entre 30 g y 49 g y la mascota se acerca (< 20 cm por 1 s),
      el motor da hasta 3 vueltas para agregar comida y corta al instante si llega a 50 g.
  - Barra de progreso corregida:
    * Calcula el porcentaje real hacia la meta actual (30 g en Fase 1 o 50 g en Fase 2)
      esperando lecturas válidas del HX711 para que nunca quede trabada en 0%.
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "HX711.h"
#include <Stepper.h>

// ================== CONFIGURACIÓN ==================

const float PESO_BASE_G       = 30.0;  // Fase 1: mínimo que siempre debe tener el plato
const float PESO_MAXIMO_G     = 50.0;  // Fase 2: tope máximo al servir por presencia
const float DISTANCIA_MAX_CM  = 20.0;  // Distancia de detección de la mascota
const unsigned long ESPERA_ENTRE_PORCIONES_MS = 30000; // 30 segundos de espera/cooldown
const unsigned long TIEMPO_PRESENCIA_MS       = 1000;  // 1 segundo frente al sensor

// Factor de calibración real obtenido en Etapa 2
float FACTOR_CALIBRACION = 441.72;

// Detección de tolva vacía
const float AUMENTO_MINIMO_G   = 3.0;   // Mínimo que debe subir el plato para considerar que cayó comida
const int   FALLOS_PARA_ALERTA = 3;     // Intentos seguidos sin que caiga comida
const unsigned long REINTENTO_ALERTA_MS = 5UL * 60UL * 1000UL; // 5 minutos

// Motor 28BYJ-48
const int PASOS_POR_VUELTA    = 2048;  // Pasos reales por vuelta (no cambiar)
const int VUELTAS_POR_PORCION = 3;     // Tope de seguridad: 3 vueltas por ciclo
const int VELOCIDAD_RPM       = 10;    // Velocidad segura (entre 5 y 15 RPM)
const int PASOS_POR_TRAMO     = 128;   // Cada 128 pasos lee balanza y actualiza pantalla

// ============================ PINES ============================

const int OLED_SDA  = 21;
const int OLED_SCL  = 22;
const int TRIG      = 5;
const int ECHO      = 18;
const int HX_DT     = 19;
const int HX_SCK    = 23;
const int IN1       = 13;
const int IN2       = 12;
const int IN3       = 14;
const int IN4       = 27;
const int LED_VERDE = 2;
const int LED_ROJO  = 4;

// ============================ OBJETOS ============================

Adafruit_SSD1306 pantalla(128, 64, &Wire, -1);
Stepper motor(PASOS_POR_VUELTA, IN1, IN3, IN2, IN4);
HX711 balanza;

// ============================ ESTADO ============================

enum Estado { LISTO, ESPERANDO, TOLVA_VACIA, ERROR_BALANZA };
Estado estado = LISTO;

unsigned long ultimaActividad = 0;     // Última descarga o última vez que estuvo el gato
unsigned long inicioPresencia = 0;     // Cuándo se acercó la mascota
unsigned long inicioAlerta    = 0;
int   fallosSeguidos          = 0;
float pesoAntesDeServir       = 0;
bool  hayPantalla             = false;
bool  primerArranque          = true;
unsigned long porcionesServidas = 0;

// ============================ SENSORES ============================

float medirDistancia() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);

  long duracion = pulseIn(ECHO, HIGH, 30000);
  if (duracion == 0) return -1;
  return duracion * 0.0343 / 2.0;
}

// Lee el peso promediando 'muestras' lecturas válidas del HX711
float medirPeso(int muestras = 5) {
  if (!balanza.wait_ready_timeout(300)) return NAN;
  float p = balanza.get_units(muestras);
  if (p < 0 && p > -5) p = 0;
  return p;
}

// ============================ MOTOR ============================

void apagarMotor() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

// ============================ PANTALLA ============================

void dibujarTitulo(const char *titulo) {
  pantalla.clearDisplay();
  pantalla.setTextColor(SSD1306_WHITE);
  pantalla.setTextSize(1);
  pantalla.setCursor(0, 0);
  pantalla.print(titulo);
  pantalla.drawLine(0, 11, 127, 11, SSD1306_WHITE);
}

void mostrarBarra(int porcentaje, float pesoVivo, float metaG, const char *modo) {
  if (!hayPantalla) return;
  dibujarTitulo(modo);

  pantalla.drawRect(4, 20, 120, 14, SSD1306_WHITE);
  int ancho = map(constrain(porcentaje, 0, 100), 0, 100, 0, 116);
  pantalla.fillRect(6, 22, ancho, 10, SSD1306_WHITE);

  pantalla.setCursor(0, 40);
  pantalla.print("Progreso: ");
  pantalla.print(porcentaje);
  pantalla.print("%");

  pantalla.setCursor(0, 52);
  pantalla.print("Plato: ");
  pantalla.print(pesoVivo, 0);
  pantalla.print("/");
  pantalla.print(metaG, 0);
  pantalla.print(" g");

  pantalla.display();
}

void mostrarReposo(float peso, float distancia) {
  if (!hayPantalla) return;

  switch (estado) {
    case LISTO:
      if (!isnan(peso) && peso >= PESO_MAXIMO_G) dibujarTitulo("Plato completo (50g)");
      else dibujarTitulo("Listo para servir");
      break;
    case ESPERANDO:     dibujarTitulo("En espera (Cooldown)"); break;
    case TOLVA_VACIA:   dibujarTitulo("! SIN COMIDA !"); break;
    case ERROR_BALANZA: dibujarTitulo("! ERROR BALANZA !"); break;
  }

  pantalla.setCursor(0, 18);
  pantalla.print("Plato: ");
  if (isnan(peso)) pantalla.print("--");
  else { pantalla.print(peso, 0); pantalla.print(" g"); }

  pantalla.setCursor(0, 30);
  pantalla.print("Mascota: ");
  if (distancia > 0 && distancia <= DISTANCIA_MAX_CM) pantalla.print("cerca");
  else pantalla.print("lejos");

  pantalla.setCursor(0, 46);
  if (estado == ESPERANDO) {
    unsigned long pasado = millis() - ultimaActividad;
    unsigned long falta = pasado < ESPERA_ENTRE_PORCIONES_MS
                          ? (ESPERA_ENTRE_PORCIONES_MS - pasado) / 1000 + 1 : 0;
    pantalla.print("Espera: ");
    pantalla.print(falta);
    pantalla.print(" s");
  } else if (estado == TOLVA_VACIA) {
    pantalla.print("Recargar la tolva");
  } else if (estado == ERROR_BALANZA) {
    pantalla.print("Revisar DT y SCK");
  } else {
    pantalla.print("Porciones hoy: ");
    pantalla.print(porcionesServidas);
  }
  pantalla.display();
}

// ============================ SERVIR ============================

void servirHastaMeta(float pesoActual, float metaG, const char *tituloModo) {
  Serial.print(">> ");
  Serial.print(tituloModo);
  Serial.print(" hasta ");
  Serial.print(metaG, 0);
  Serial.println(" g");

  pesoAntesDeServir = pesoActual < 0 ? 0 : pesoActual;
  float pesoEnVivo = pesoAntesDeServir;

  digitalWrite(LED_ROJO, LOW);
  digitalWrite(LED_VERDE, HIGH);

  int totalPasos = PASOS_POR_VUELTA * VUELTAS_POR_PORCION;
  int pasosHechos = 0;

  // Si es Fase 1 (base 30g), damos más margen de vueltas por si el plato estaba en 0g
  if (metaG == PESO_BASE_G) {
    totalPasos = PASOS_POR_VUELTA * 15;
  }

  while (pasosHechos < totalPasos) {
    // Leemos 2 muestras esperando que el HX711 esté listo (evita que quede en 0%)
    float lectura = medirPeso(2);
    if (!isnan(lectura)) {
      pesoEnVivo = lectura < 0 ? 0 : lectura;
    }

    int porc = 0;
    if (metaG > 0) {
      porc = constrain((int)((pesoEnVivo / metaG) * 100.0), 0, 100);
    }
    mostrarBarra(porc, pesoEnVivo, metaG, tituloModo);

    // Corta al instante cuando alcanza la meta (30g en Fase 1 o 50g en Fase 2)
    if (pesoEnVivo >= metaG) {
      Serial.print("   Meta alcanzada (");
      Serial.print(pesoEnVivo, 0);
      Serial.println(" g), deteniendo motor...");
      break;
    }

    int tramo = min(PASOS_POR_TRAMO, totalPasos - pasosHechos);
    motor.step(tramo);
    pasosHechos += tramo;
  }
  apagarMotor();

  digitalWrite(LED_VERDE, LOW);
  porcionesServidas++;
  ultimaActividad = millis();
  estado = ESPERANDO;

  // Esperar que caigan los últimos granos antes de verificar aumento
  delay(1200);
  float pesoDespues = medirPeso(5);
  if (!isnan(pesoDespues)) {
    float aumento = pesoDespues - pesoAntesDeServir;
    Serial.print("   El plato subio ");
    Serial.print(aumento, 0);
    Serial.println(" g");

    if (aumento < AUMENTO_MINIMO_G && pesoDespues < metaG) {
      fallosSeguidos++;
      Serial.print("   No cayo comida (");
      Serial.print(fallosSeguidos);
      Serial.print("/");
      Serial.print(FALLOS_PARA_ALERTA);
      Serial.println(")");
      if (fallosSeguidos >= FALLOS_PARA_ALERTA) {
        estado = TOLVA_VACIA;
        inicioAlerta = millis();
        Serial.println("!! Tolva vacia: se deja de servir");
      }
    } else {
      fallosSeguidos = 0;
    }
  }
}

// ============================ CALIBRACIÓN ============================

void calibrar() {
  Serial.println("\n=== CALIBRACION ===");
  Serial.println("1. Saca todo del plato y manda cualquier tecla.");
  while (!Serial.available()) delay(10);
  while (Serial.available()) Serial.read();
  balanza.tare();
  Serial.println("2. Pone un objeto de peso conocido (ej: 500 g), escribi su peso y Enter.");
  while (!Serial.available()) delay(10);
  float conocido = Serial.parseFloat();
  while (Serial.available()) Serial.read();
  if (conocido <= 0) { Serial.println("Peso invalido."); return; }
  float lectura = balanza.get_value(10);
  FACTOR_CALIBRACION = lectura / conocido;
  balanza.set_scale(FACTOR_CALIBRACION);
  Serial.print("Nuevo FACTOR_CALIBRACION = ");
  Serial.println(FACTOR_CALIBRACION, 4);
}

void leerComandos() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == 't' || c == 'T') {
    balanza.tare();
    Serial.println("Balanza puesta en cero");
  } else if (c == 'c' || c == 'C') {
    calibrar();
  }
}

// ============================ SETUP ============================

void setup() {
  Serial.begin(115200);
  Serial.println("\n=== DISPENSER SMART (BASE 30g + MAX 50g) ===");

  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_ROJO, OUTPUT);
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  apagarMotor();
  motor.setSpeed(VELOCIDAD_RPM);

  Wire.begin(OLED_SDA, OLED_SCL);
  hayPantalla = pantalla.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (hayPantalla) {
    dibujarTitulo("Dispenser Smart");
    pantalla.setCursor(0, 24);
    pantalla.print("Calibrando cero...");
    pantalla.display();
  }

  balanza.begin(HX_DT, HX_SCK);
  if (balanza.wait_ready_timeout(2000)) {
    balanza.set_scale(FACTOR_CALIBRACION);
    balanza.tare();
    Serial.println("[OK] Balanza en cero");
  } else {
    Serial.println("[ERROR] La balanza no responde");
    estado = ERROR_BALANZA;
  }

  // Pequeña pausa de cortesía al encender antes del llenado base de 30g
  delay(2500);
}

// ============================ LOOP ============================

void loop() {
  leerComandos();

  float peso = medirPeso(5);
  float distancia = medirDistancia();
  bool mascotaCerca = (distancia > 0 && distancia <= DISTANCIA_MAX_CM);

  // Si la mascota está comiendo/cerca, actualizamos el reloj de presencia
  if (mascotaCerca) {
    if (inicioPresencia == 0) inicioPresencia = millis();
  } else {
    inicioPresencia = 0;
  }
  bool presenciaConfirmada = (inicioPresencia != 0 &&
                             (millis() - inicioPresencia >= TIEMPO_PRESENCIA_MS));

  // --- Gestión de estados ---
  if (isnan(peso)) {
    estado = ERROR_BALANZA;
  } else if (estado == ERROR_BALANZA) {
    estado = LISTO;
  }

  if (estado == ESPERANDO && (millis() - ultimaActividad >= ESPERA_ENTRE_PORCIONES_MS)) {
    estado = LISTO;
  }

  if (estado == TOLVA_VACIA) {
    bool pasoTiempo = (millis() - inicioAlerta >= REINTENTO_ALERTA_MS);
    bool subioPeso  = (!isnan(peso) && peso >= pesoAntesDeServir + AUMENTO_MINIMO_G);
    if (pasoTiempo || subioPeso) {
      fallosSeguidos = 0;
      estado = LISTO;
    }
  }

  // --- Control de LEDs ---
  if (estado == TOLVA_VACIA || estado == ERROR_BALANZA) {
    digitalWrite(LED_ROJO, (millis() / 300) % 2);
  } else {
    digitalWrite(LED_ROJO, estado == ESPERANDO ? HIGH : LOW);
  }

  // --- Lógica de dosificación en 2 Fases ---
  if (estado == LISTO && !isnan(peso)) {

    // FASE 1: Relleno base hasta 30 g
    // - Si es el primer arranque, llena directo a 30 g.
    // - Si ya está funcionando, solo rellena cuando el gato NO está cerca (ya se fue)
    //   y ya pasó el tiempo de espera (estado == LISTO).
    if (peso < PESO_BASE_G && (primerArranque || !mascotaCerca)) {
      primerArranque = false;
      servirHastaMeta(peso, PESO_BASE_G, "Llenado base (30g)");
    }

    // FASE 2: Ración extra por presencia hasta máximo 50 g
    // - Si el plato tiene entre 30 g y 49 g y la mascota se acerca 1 segundo,
    //   da hasta 3 vueltas y corta al instante si llega a 50 g.
    else if (peso >= PESO_BASE_G && peso < PESO_MAXIMO_G && presenciaConfirmada) {
      primerArranque = false;
      servirHastaMeta(peso, PESO_MAXIMO_G, "Racion extra (50g)");
      inicioPresencia = 0;
    }
  }

  // --- Monitor Serie ---
  Serial.print("Peso: ");
  if (isnan(peso)) Serial.print("--"); else { Serial.print(peso, 0); Serial.print(" g"); }
  Serial.print(" | Dist: ");
  if (distancia < 0) Serial.print("--"); else { Serial.print(distancia, 1); Serial.print(" cm"); }
  Serial.print(" | Estado: ");
  const char *nombres[] = { "LISTO", "ESPERANDO", "TOLVA VACIA", "ERROR BALANZA" };
  Serial.println(nombres[estado]);

  mostrarReposo(peso, distancia);
  delay(200);
}