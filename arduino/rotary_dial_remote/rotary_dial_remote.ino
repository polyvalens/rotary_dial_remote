/*
 * File: rotary_dial_remote.ino
 * 
 * Purpose: 
 * Entry for WIZnet Ethernet Hat Contest 2022.
 * Rotary-dial phone as MQTT remote for home automation system.
 *
 * Author: Clemens Valens
 * Date: 4/20/2022
 * 
 * Board: W5100S-EVB-Pico
 * Arduino IDE: 1.8.19
 *
 * Boards Package:
 * - Raspberry Pi Pico/RP2040
 *   Earle F. Philhower, III version 1.13.1
 *
 * Libraries:
 * - WIZnet Pico
 * - DFRobotDFPlayerMini, version 1.0.5
 *
 * License: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>
#include <wiznet_pico.h>

// MP3 player
#include <DFRobotDFPlayerMini.h>
DFRobotDFPlayerMini mp3;
#define MP3_SERIAL  Serial1

// Buffer
#define ETHERNET_BUF_MAX_SIZE  (1024 * 2)
#define MQTT_PUBLISH_PAYLOAD_SIZE  (128)

// mDNS
char MQTT_SERVER_NAME[] = "hassio.local";
uint8_t mdns_buffer[MAX_MDNS_BUF_SIZE];

// Network
static wiz_NetInfo g_net_info =
{
  .mac = {0x00, 0x08, 0xdc, 0x01, 0x04, 0x66}, // 0x0008dc is WIZnet's OUI (organizationally unique identifier)/vendor code.
  .ip = {192, 168, 2, 200}, // My IP address
  .sn = {255, 255, 255, 0}, // Subnet Mask
  .gw = {192, 168, 2, 1}, // My gateway
  .dns = {8, 8, 8, 8}, // DNS server
  .dhcp = NETINFO_STATIC // DHCP enable/disable
};

// MQTT
#define MQTT_SOCKET (0)
#define MQTT_PORT (1883)
char MQTT_client_id[] = "rotarydial";
char MQTT_username[] = "raspberrypi";
char MQTT_password[] = "raspberry";
char MQTT_subscribe_topic[] = "rotarydial_to";
char MQTT_publish_topic[MQTT_PUBLISH_PAYLOAD_SIZE] = { 0 };
char MQTT_publish_payload[MQTT_PUBLISH_PAYLOAD_SIZE] = { 0 };
#define MQTT_PUBLISH_PERIOD (1000 * 10) /* 10 seconds */
#define MQTT_KEEP_ALIVE (60) /* 60 milliseconds */
uint8_t g_mqtt_send_buf[ETHERNET_BUF_MAX_SIZE] = { 0 };
uint8_t g_mqtt_recv_buf[ETHERNET_BUF_MAX_SIZE] = { 0 };
Network g_mqtt_network;
MQTTClient g_mqtt_client;
MQTTPacket_connectData g_mqtt_packet_connect_data = MQTTPacket_connectData_initializer;
MQTTMessage g_mqtt_message;
void message_arrived(MessageData *msg_data);

// Timer
#define DEFAULT_TIMEOUT  (1000) /* 1 second */
volatile uint32_t g_msec_cnt = 0;
void repeating_timer_callback(void);
time_t my_millis(void);
uint32_t t_previous = 0;

// Rotary dial phone
#define PHONE_POLL_PERIOD  (0) /* milliseconds */
#define DEBOUNCE_HISTORY_LENGTH  (15)
#define PHONE_NUMBER_LENGTH  (100)

// Dial & hook
const int hook_pin = A0;
const int dialing_pin = A1;
const int pulse_pin = A2;
// Bell
#define BELL_HV_HALF_PERIOD  (10)
#define BELL_ONE_SECOND  (1000)
const int bell_p_pin = 2; // Bell voltage inverter pin 1
const int bell_n_pin = 3; // Bell voltage inverter pin 2
volatile int bell_50hz = BELL_HV_HALF_PERIOD;
volatile int bell_timer = 0; // Bell off.

typedef struct
{
  uint16_t value;
  uint16_t low;
  uint16_t high;
  bool state;
  uint8_t i;
  uint16_t history[DEBOUNCE_HISTORY_LENGTH];
}
debounce_state_t;

debounce_state_t hook_state = { 1000, 250, 1000, true, 0 };
debounce_state_t dialing_state = { 350, 250, 350, true, 0 };
debounce_state_t pulse_state = { 350, 250, 350, true, 0 };

// MQTT receive callback
void message_arrived(MessageData *msg_data)
{
  MQTTMessage *message = msg_data->message;
  if (strncmp("{'bell':'1'}",(char*)message->payload,(uint32_t)message->payloadlen)==0)
  {
    bell_on();
  }
  else if (strncmp("{'mp3_file':'",(char*)message->payload,12)==0)
  {
    mp3.play(((char*)message->payload)[13]-'0');
  }
  else
  {
    Serial.printf("%.*s\n",(uint32_t)message->payloadlen,(uint8_t *)message->payload);
  }
}

// Timer callback
void repeating_timer_callback(void)
{
  g_msec_cnt++;
  MilliTimer_Handler(); // Must be called every 1 ms? See mqtt_interface.h
  if ((g_msec_cnt%1000)==0)
  {
    MDNS_time_handler(); // 1 second
    //Serial.println('*');
  }

  if (bell_timer>0)
  {
    bell_timer -= 1;
    if (bell_timer<=0)
    {
      bell_off();
    }
    else
    {
      // Generate 50 Hz square wave.
      bell_50hz -= 1;
      if (bell_50hz<=0)
      {
        bell_50hz = BELL_HV_HALF_PERIOD; // 10 ms
        bell_toggle();
      }
    }
  }
}

time_t my_millis(void)
{
  return g_msec_cnt;
}

uint32_t dial_idle_timer = 0;
bool dial_idle_timer_run = false;

void dial_idle_timer_tick(void)
{
  if (dial_idle_timer_run==true)
  {
    dial_idle_timer++;
  }
}

void dial_idle_timer_start(void)
{
  dial_idle_timer = 0;
  dial_idle_timer_run = true;
}

uint32_t dial_idle_timer_stop(void)
{
  dial_idle_timer_run = false;
  return dial_idle_timer;
}

bool debounce(uint16_t value, debounce_state_t *p_state)
{
  // Add new value.
  p_state->history[p_state->i] = value;
  p_state->i += 1;
  if (p_state->i>=DEBOUNCE_HISTORY_LENGTH) p_state->i = 0;
  
  // Calculate average.
  uint32_t sum = 0;
  int i;
  for (i=0; i<DEBOUNCE_HISTORY_LENGTH; i++)
  {
    sum += p_state->history[i];
  }
  p_state->value = sum/i;
  
  // Determine state.
  if (p_state->value<p_state->low) p_state->state = false;
  else if (p_state->value>p_state->high) p_state->state = true;
  return p_state->state;
}

char to_ascii(uint8_t val)
{
  if (val>9) val = 9;
  return '0' + val;
}

// French phone model S63
#define __HAS_Z__
// wiznet = 9 444 000 66 33 8
char alphadial_table[10][3] =
{
  { '.', '.', '.' }, // 1
  { 'a', 'b', 'c' }, // 2
  { 'd', 'e', 'f' }, // 3
  { 'g', 'h', 'i' }, // 4
  { 'j', 'k', 'l' }, // 5
  { 'm', 'n', '.' }, // 6, two characters only
  { 'p', 'r', 's' }, // 7
  { 't', 'u', 'v' }, // 8
  { 'w', 'x', 'y' }, // 9
#ifdef __HAS_Z__
  { 'o', 'q', 'z' }, // 0
#else
  { 'o', 'q', '.' }, // 0, two characters only, 'z' could go here
#endif /* __HAS_Z__ */
};

// According to https://en.wikipedia.org/wiki/Rotary_dial
/*char alphadial_table[10][3] =
{
  { '.', '.', '.' }, // 1
  { 'a', 'b', 'c' }, // 2
  { 'd', 'e', 'f' }, // 3
  { 'g', 'h', 'i' }, // 4
  { 'j', 'k', 'l' }, // 5
  { 'm', 'n', 'o' }, // 6
  { 'p', 'r', 's' }, // 7
  { 't', 'u', 'v' }, // 8
  { 'w', 'x', 'y' }, // 9
  { '.', '.', '.' }, // 0
};*/

// The frequency of the letters of the alphabet in English
// https://www3.nd.edu/~busiforc/handouts/cryptography/letterfrequencies.html
// With email & URL characters.
/*char alphadial_table[10][3] =
{
  { 'e', 'a', 'r' }, // 1
  { 'i', 'o', 't' }, // 2
  { 'n', 's', 'l' }, // 3
  { 'c', 'u', 'd' }, // 4
  { 'p', 'm', 'h' }, // 5
  { 'g', 'b', 'f' }, // 6
  { 'y', 'w', 'k' }, // 7
  { 'v', 'w', 'z' }, // 8
  { 'j', 'q', '@' }, // 9
  { '.', '/', ':' }, // 0
};*/

char alphadial_str[PHONE_NUMBER_LENGTH];
uint8_t alphadial_index = 0;

void build_message(void)
{
  g_mqtt_message.qos = QOS0;
  g_mqtt_message.retained = 0;
  g_mqtt_message.dup = 0;
  g_mqtt_message.payload = MQTT_publish_payload;
  g_mqtt_message.payloadlen = strlen((const char *)g_mqtt_message.payload);
}

int32_t publish(char *p_topic, char *p_payload)
{
  // Set topic.
  sprintf(MQTT_publish_topic,"rotarydial/%s",p_topic);
  // Build payload.
  sprintf(MQTT_publish_payload,"{\"%s\":\"%s\"}",p_topic,p_payload);
  build_message();
  int32_t retval = MQTTPublish(&g_mqtt_client,MQTT_publish_topic,&g_mqtt_message);
  if (retval<0)
  {
    Serial.printf("[publish] failed with code %d\n",retval);
  }
  return retval;
}

void bell_on(void)
{
  bell_timer = BELL_ONE_SECOND;
  bell_50hz = BELL_HV_HALF_PERIOD;
  bell_toggle();
}

void bell_off(void)
{
  digitalWrite(bell_p_pin,LOW);
  digitalWrite(bell_n_pin,LOW);
}

void bell_toggle(void)
{
  static bool state = false;
  if (state==false)
  {
    digitalWrite(bell_p_pin,HIGH);
    digitalWrite(bell_n_pin,LOW);
    state = true;
  }
  else
  {
    digitalWrite(bell_p_pin,LOW);
    digitalWrite(bell_n_pin,HIGH);
    state = false;
  }
}

void mp3_init(void)
{
  MP3_SERIAL.begin(9600);

  Serial.print("MP3 init ");
  if (mp3.begin(MP3_SERIAL)==false)
  {
    Serial.println("failed");
  }
  else 
  {
    Serial.println("OK");
    mp3.volume(30);  // Set volume (0 to 30)
    mp3.play(1);  // Play the first mp3.
  }
}

void setup(void)
{
  int32_t retval = 0;

  Serial.begin(115200);
  //while (!Serial); // debug only.

  // Initialize bell inverter pins.
  pinMode(bell_p_pin,OUTPUT);
  pinMode(bell_n_pin,OUTPUT);

  memset(alphadial_str,0,sizeof(alphadial_str));
  alphadial_index = 0;

  wizchip_spi_initialize();
  wizchip_cris_initialize();
  wizchip_reset();
  wizchip_initialize();
  wizchip_check();
  wizchip_1ms_timer_initialize(repeating_timer_callback);
  
  network_initialize(g_net_info);
  print_network_information(g_net_info);
  
  // Try to discover IP of MQTT broker using mDNS.
  Serial.printf("Doing mDNS to find MQTT broker '%s'\r\n",MQTT_SERVER_NAME);
  MDNS_init(MQTT_SOCKET+1,mdns_buffer);
  uint8_t ip_from_mdns[4];
  retval = MDNS_run((uint8_t*)MQTT_SERVER_NAME,ip_from_mdns);
  if (retval==1)
  {
    Serial.printf("%s resolves to %d.%d.%d.%d\r\n",MQTT_SERVER_NAME,ip_from_mdns[0],ip_from_mdns[1],ip_from_mdns[2],ip_from_mdns[3]);
  }
  else
  {
    Serial.println("mDNS failed, cannot connect to MQTT broker.");
    while (1);
  }
  
  Serial.printf("MQTT network connect ");
  NewNetwork(&g_mqtt_network,MQTT_SOCKET);
  retval = ConnectNetwork(&g_mqtt_network,ip_from_mdns,MQTT_PORT);
  if (retval!=1)
  {
    Serial.printf("failed\n");
    while (1);
  }
  Serial.printf("OK\n");
  
  // Initialize MQTT client.
  MQTTClientInit(&g_mqtt_client,&g_mqtt_network,DEFAULT_TIMEOUT,g_mqtt_send_buf,ETHERNET_BUF_MAX_SIZE,g_mqtt_recv_buf,ETHERNET_BUF_MAX_SIZE);
  
  // Connect to the MQTT broker.
  g_mqtt_packet_connect_data.MQTTVersion = 3;
  g_mqtt_packet_connect_data.cleansession = 1;
  g_mqtt_packet_connect_data.willFlag = 0;
  g_mqtt_packet_connect_data.keepAliveInterval = MQTT_KEEP_ALIVE;
  g_mqtt_packet_connect_data.clientID.cstring = MQTT_client_id;
  g_mqtt_packet_connect_data.username.cstring = MQTT_username;
  g_mqtt_packet_connect_data.password.cstring = MQTT_password;
  
  Serial.printf("MQTT broker connect ");
  retval = MQTTConnect(&g_mqtt_client,&g_mqtt_packet_connect_data);
  if (retval<0)
  {
    Serial.printf("failed: %d\n", retval);
    while (1);
  }
  Serial.printf("OK\n");
   
  // Subscribe
  Serial.printf("MQTT subscribe ");
  retval = MQTTSubscribe(&g_mqtt_client,MQTT_subscribe_topic,QOS0,message_arrived);
  if (retval<0)
  {
    Serial.printf("failed : %d\n", retval);
    while (1);
  }
  Serial.printf("OK\n");

  mp3_init(); // Don't call too early, MP3 module needs time to boot.
  
  Serial.println("Setup completed.");
  t_previous = my_millis();
}

void loop(void)
{
  static bool hook_down = true;
  static bool dial_idle = true;
  static bool pulse = true;
  static bool pulse_last = true;
  static uint8_t pulse_count = 0;
  static uint8_t pulse_count_previous = 0;
  static uint8_t ch = 0;
  static int16_t milliseconds = 0;
  static int16_t deci_seconds = 0;
  static uint8_t digit_count = 0;
  static char number_str[PHONE_NUMBER_LENGTH+1];
  int32_t retval = 0;

  retval = MQTTYield(&g_mqtt_client,g_mqtt_packet_connect_data.keepAliveInterval);
	if (retval<0)
	{
		Serial.printf(" Yield error : %d\n", retval);
		while (1);
	}

  uint32_t t_now = my_millis();
  if (t_now>t_previous+PHONE_POLL_PERIOD)
  {
    t_previous = t_now;
    dial_idle_timer_tick();
    
    // Read & debounce phone switches.
    hook_down = debounce(analogRead(hook_pin),&hook_state);
    dial_idle = debounce(analogRead(dialing_pin),&dialing_state);
    pulse = debounce(analogRead(pulse_pin),&pulse_state);

    // Dialing in progress?
    if (dial_idle==false)
    {
      // Dialing in progress.
      dial_idle_timer_stop();
      // Count pulses.
      if (pulse==true && pulse!=pulse_last)
      {
        // Count rising edge.
        pulse_count++;
      }
      pulse_last = pulse;
    }
    else
    {
      // Dialing terminated with at least 1 pulse?
      if (pulse_count!=0)
      {
        Serial.printf("dialed %d\n",pulse_count);
        // Dialing hook up or hook down?
        if (hook_down==false)
        {
          // Hook up, collect number.
          if (digit_count<PHONE_NUMBER_LENGTH)
          {
            number_str[digit_count] = to_ascii(pulse_count-1);
            digit_count += 1;
          }
        }
        else
        {
          // Hook down, publish single digits.
          char str[5];
          publish((char*)"digit",itoa(pulse_count-1,str,10));
        }

        // Alphadial: translate digits to letters on the fly.
        if (pulse_count_previous==pulse_count & dial_idle_timer<1000)
        {
          // Same digit within timeout, move to the next character for this digit.
          // The timeout must be rather long to give the user the time to move
          // his/her finger to the dial and turn it to select the next letter.
          ch = (ch+1)%3; // 3 letters per digit.
          #ifdef __HAS_Z__
          if (ch==2 && pulse_count==6)
          #else
          if (ch==2 && (pulse_count==6 || pulse_count==10))
          #endif /* __HAS_Z__ */
          {
            // 6 & 0 have only two letters.
            ch = 0;
          }
          alphadial_str[alphadial_index-1] = alphadial_table[pulse_count-1][ch];
        }
        else
        {
          // Different digit or timeout, go to the first character.
          ch = 0;
          alphadial_str[alphadial_index] = alphadial_table[pulse_count-1][ch];
          alphadial_index += 1;
          alphadial_str[alphadial_index] = 0; // Zero terminate.
        }
        pulse_count_previous = pulse_count;
        
        // Clear pulse count, return to idle.
        pulse_count = 0;
        dial_idle_timer_start();
      }
    }

    // Hook down and we collected a number?
    if (hook_down==true && digit_count!=0)
    {
      // Publish number.
      number_str[digit_count] = 0; // Zero terminate.
      publish((char*)"number",number_str);

      // Publish string.
      publish((char*)"string",alphadial_str);

      // Return to idle.
      alphadial_index = 0;
      digit_count = 0;
    }

    milliseconds += 1; //PHONE_POLL_PERIOD;
    if (milliseconds>100)
    {
      milliseconds -= 100;
      deci_seconds++;
      if (deci_seconds>100)
      {
        deci_seconds -= 100;
        Serial.printf("hook: %d\t",analogRead(hook_pin));
        Serial.printf("dialing: %d\t",analogRead(dialing_pin));
        Serial.printf("pulse: %d\n",analogRead(pulse_pin));
        //Serial.printf("hook=%d, dialing=%d, pulse=%d, ",hook_down,dial_idle,pulse);
        //Serial.printf("digit_count=%d, pulse_count=%d\n",digit_count,pulse_count);
      }
    }
  }
}
