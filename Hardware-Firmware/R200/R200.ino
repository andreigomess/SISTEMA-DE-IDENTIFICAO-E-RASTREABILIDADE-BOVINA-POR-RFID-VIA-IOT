/**
 * R200 RFID demonstration
 * Copyright (c) 2023 Alastair Aitchison, Playful Technology
 */ 

// INCLUDES

#include "R200.h"
#include <WiFi.h>
#include <PubSubClient.h>

//Credenciais de rede (wifi)
const char* ssid = "nome da minha rede wifi";
const char* password = "senha da minha rede"; 
const char* mqtt_server = "xxx.xxx.x.x"; //Ip do computador que receberá os dados

WiFiClient espClient;
PubSubClient client(espClient);

// GLOBALS
unsigned long lastResetTime = 0;
R200 rfid;

//Variáveis para o sistema de tolerância (evitar mesma leitura repetidamente)
const unsigned long tempo_tolerancia = 8000; //8000 milissegundos (8 segundos)
const int quantia_tags_max = 10; //o máximo de tags que será lida simultaneamente

//struct para apontar para cada tag (ou animal com a tag)
struct AnimalDetectado{
  uint8_t uid[12]; //tamanho da tag
  unsigned long ultimaLeitura;
  bool ativo; //se for falsa, então ocorre a releitura
};

AnimalDetectado lista_animais[quantia_tags_max];

// Função auxiliar para verificar se o ID da tag está vazio
bool is_uid_Empty(uint8_t *uidArray) {
  for (int i = 0; i < 12; i++) {
    if (uidArray[i] != 0) return false;
  }

  return true;
}

//Funções de rede:
void setup_wifi(){
  delay(10);
  Serial.println();
  Serial.print("Conectando na rede: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    //enquanto ainda não conectar
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi conectado. IP: ");
  Serial.println(WiFi.localIP());
}

void reconnect(){
  while (!client.connected()) {
    Serial.print("Tentando conexao MQTT...");

    if (client.connect("ESP32_Curral_01")) {
      Serial.println("Conectado ao Broker!");

    } else {
      Serial.print("Falha, rc=");
      Serial.print(client.state());
      Serial.println(" Tentando novamente em 5 segundos");

      delay(5000);
    }
  }
}

//Para enviar os dados:
void publicarEventoMQTT(uint8_t *uidArray, String evento) {
  if (client.connected()) {
    String epcLido = "0x";

    for(int j = 0; j < 12; j++){
      if(uidArray[j] < 0x10) epcLido += "0";

      epcLido += String(uidArray[j], HEX);
    }

    epcLido.toUpperCase();

    // Monta o pacote JSON
    String jsonPayload = "{\"epc\":\"" + epcLido + "\", \"evento\":\"" + evento + "\"}";
    
    // Envia para o tópico do curral
    client.publish("fazenda/curral1/leituras", jsonPayload.c_str()); //caminho onde ficará guardado as leituras
  }
}

void setup() {
  Serial.begin(115200);
  // Intitialise Serial connection (for debugging)
  //Serial.println(__FILE__ __DATE__);

  setup_wifi();
  client.setServer(mqtt_server, 1883);

  rfid.begin(&Serial2, 115200, 16, 17);

  //Inicializar a lista de animais como vazia:
  //12 ou "quantias_tags_max" ?
  for(int i = 0; i < 12; i++){
    lista_animais[i].ativo = false;
  }

  Serial.println("Sistema de Rastreio (Múltiplas tags) ligado");

  // Get info
  //rfid.dumpModuleInfo();
}

void loop() {
  // Mantém o MQTT rodando
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  rfid.loop();

  // 1. Verifica se a biblioteca leu alguma tag neste exato ciclo
  if (!is_uid_Empty(rfid.uid)) {
    bool tag_conhecida = false;

    //Procurar se a tag está na lista de ativas no momento:
    for(int i = 0; i < quantia_tags_max; i++){
      if(lista_animais[i].ativo && memcmp(lista_animais[i].uid, rfid.uid, 12) == 0){
        //Se for verdadeiro, então entra aqui (a tag já estava na lista)
        //Atualiza o cronômetro de vida
        lista_animais[i].ultimaLeitura = millis();
        tag_conhecida = true;
        break;
      }
    }
    
    //Se é um novo animal, registra entrada:
    if(!tag_conhecida){
      for(int i = 0; i < quantia_tags_max; i++){
        //procurar um local vazio no array (lista_animais) para colocar o animal ali
        
        if(!lista_animais[i].ativo){
          memcpy(lista_animais[i].uid, rfid.uid, 12);
          lista_animais[i].ultimaLeitura = millis();
          lista_animais[i].ativo = true;

          // Publica no MQTT em vez de apenas imprimir
          publicarEventoMQTT(lista_animais[i].uid, "ENTRADA");
          //Serial.println("Nova tag publicada via MQTT.");

          Serial.print("[ENTRADA] Animal detectado: 0x");

          for(int j = 0; j < 12; j++){
            Serial.print(lista_animais[i].uid[j] < 0x10 ? "0" : "");
            Serial.print(lista_animais[i].uid[j], HEX);
          }

          Serial.println();
          break;
        }
      }
    }

    // CRÍTICO: Limpa o buffer do R200 imediatamente após ler
    memset(rfid.uid, 0, 12);
  }

  // 2. Verifica saídas das tags (timeouts)
  for(int i = 0; i < quantia_tags_max; i++){
    if(lista_animais[i].ativo && (millis() - lista_animais[i].ultimaLeitura > tempo_tolerancia)){
      // Publica a saída no MQTT
      publicarEventoMQTT(lista_animais[i].uid, "SAIDA");
      //Serial.println("Saida de tag publicada via MQTT.");

      Serial.print("[SAÍDA] Animal confirmou saída: 0x");
      
      for(int j = 0; j < 12; j++){
        Serial.print(lista_animais[i].uid[j] < 0x10 ? "0" : "");
        Serial.print(lista_animais[i].uid[j], HEX);
      }

      Serial.println();
      
      // Liberta o espaço no array
      lista_animais[i].ativo = false;
    }
  }

  // 3. Pede leitura ao módulo RFID a cada uma quantia de ms
  if(millis() - lastResetTime > 2000){
    rfid.poll();
    lastResetTime = millis();
  }

  // Periodically re-send the read command
  // if(millis() - lastResetTime > 1000){
  //   //  digitalWrite(LED_BUILTIN, HIGH);
  //   rfid.poll();
  //   //rfid.dumpUIDToSerial();
  //   //rfid.getModuleInfo();
  //   //  digitalWrite(LED_BUILTIN, LOW);
  //   lastResetTime = millis();
  // }
  // delay(1000);
}