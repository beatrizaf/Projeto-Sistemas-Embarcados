#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "ds18b20.h"
#include "driver/i2c.h"
#include "i2c-lcd.h"

/*Sensor de nível*/
#define TRIGGER_GPIO 0
#define ECHO_GPIO 1
#define DIST_MAX 9.59 //distância entre o sensor e o fundo do reservatório
#define DIST_MIN 2.5 //distância mínima entre o sensor e a superfície da água

#define DS18B20_GPIO 12 //Sensor temperatura

/*Leds e Buzzer*/
#define LED_G1_GPIO GPIO_NUM_9
#define LED_G2_GPIO GPIO_NUM_8
#define LED_Y1_GPIO GPIO_NUM_7
#define LED_Y2_GPIO GPIO_NUM_6
#define LED_R1_GPIO GPIO_NUM_10
#define LED_R2_GPIO GPIO_NUM_3
#define BUZZER_GPIO GPIO_NUM_2

/*Relé*/
#define RELAY_1_GPIO GPIO_NUM_20 //controla bomba d'água
#define RELAY_2_GPIO GPIO_NUM_21 //controla resistência

/*Display LCD*/
#define SDA_GPIO GPIO_NUM_4
#define SCL_GPIO GPIO_NUM_5
#define TEMP_DEFAULT 25

/*Variáveis auxiliares para a bomba d'água*/
bool pump_active = false;
int count_level = 0;

void leds_config()
{
    /*Configuração dos leds e buzzer*/
    gpio_pad_select_gpio(LED_G1_GPIO);
    gpio_pad_select_gpio(LED_G2_GPIO);
    gpio_pad_select_gpio(LED_Y1_GPIO);
    gpio_pad_select_gpio(LED_Y2_GPIO);
    gpio_pad_select_gpio(LED_R1_GPIO);
    gpio_pad_select_gpio(LED_R2_GPIO);
    gpio_pad_select_gpio(BUZZER_GPIO);
    
    gpio_set_direction(LED_G1_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_G2_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_Y1_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_Y2_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_R1_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_R2_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
}

void led_buzzer_blink(){
    /*Função que faz o buzzer e o led vermelho piscar 
    indicando algum erro*/
    gpio_set_level(BUZZER_GPIO, 1);
    gpio_set_level(LED_R2_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(BUZZER_GPIO, 0);
    gpio_set_level(LED_R2_GPIO, 0);
}

void leds_update(float percentage){
    /*Atualiza os leds de acordo com a porcentagem medida pelo sensor*/
    gpio_set_level(LED_G1_GPIO, percentage >= 96 ? 1 : 0);
    gpio_set_level(LED_G2_GPIO, percentage >= 75 ? 1 : 0);
    gpio_set_level(LED_Y1_GPIO, percentage >= 65 ? 1 : 0);
    gpio_set_level(LED_Y2_GPIO, percentage >= 50 ? 1 : 0);
    gpio_set_level(LED_R1_GPIO, percentage >= 25 ? 1 : 0);
    gpio_set_level(LED_R2_GPIO, percentage >= 10 ? 1 : 0); 
    vTaskDelay(pdMS_TO_TICKS(100));
    if(percentage <= 10){
        led_buzzer_blink();
    }
}

void turn_pump_on() {
    /*Liga a bomba d'água*/
    gpio_set_level(RELAY_1_GPIO, 0);
    pump_active = true;
    printf("Bomba d'água ligada\n");
}

void turn_pump_off() {
    /*desliga a bomba d'água*/
    gpio_set_level(RELAY_1_GPIO, 1);
    pump_active = false;
    printf("Bomba d'água desligada\n");
}

void hcsr04_config(){
    // Configura GPIOs do sensor hcsr04
    gpio_reset_pin(TRIGGER_GPIO);
    gpio_set_direction(TRIGGER_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(ECHO_GPIO);
    gpio_set_direction(ECHO_GPIO, GPIO_MODE_INPUT);
}

void hcsr04_init(){
    //Envia o sinal trigger
    gpio_set_level(TRIGGER_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(TRIGGER_GPIO, 0);

    //Espera pelo sinal echo
    while (gpio_get_level(ECHO_GPIO) == 0) { }
    uint64_t echo_start = esp_timer_get_time();

    while (gpio_get_level(ECHO_GPIO) == 1) {}
    uint64_t echo_end = esp_timer_get_time();

    uint64_t duration = (echo_end - echo_start);
    //distância em cm
    float distance = (duration * 0.0343) / 2; 
    //nível em porcentagem (0-100)
    float percentage = ((DIST_MAX - distance)/(DIST_MAX - DIST_MIN)) * 100; 

    if(percentage >= 0){ //Porcentagem válida
        if(percentage <= 95){
            count_level += 1;
            if(count_level == 3){ 
                /*Se houverem 3 leituras do nível da caixa menor que 95
                devemos ligar a bomba*/
                turn_pump_on();
            }
        }
        else{
            if(pump_active){
                /*Se a bomba estiver ligada mas a porcentagem é maior que 95
                devemos desligar a bomba*/
                turn_pump_off();
            }
            //Se a porcentagem for maior que 95, zera o contador de leituras 
            count_level = 0; 
        }
        //Indica a porcentagem atual através dos leds
        leds_update(percentage);
    }
    ESP_LOGI("hcsr04", "Distância: %.2f cm", distance);
    ESP_LOGI("hcsr04", "Nível: %.1f %%", percentage);

}

void relay_config(){
    /*Configura as GPIOs do relé*/
    gpio_pad_select_gpio(RELAY_1_GPIO);
    gpio_set_direction(RELAY_1_GPIO, GPIO_MODE_OUTPUT);

    gpio_pad_select_gpio(RELAY_2_GPIO);
    gpio_set_direction(RELAY_2_GPIO, GPIO_MODE_OUTPUT);

    /*Nível lógico 0 liga o led do relé e os dispositivos conectados ao NO
    Nível lógico 1 desliga*/
    //Relé inicia desligado
    gpio_set_level(RELAY_1_GPIO, 1);
    gpio_set_level(RELAY_2_GPIO, 1);
}


static esp_err_t i2c_master_init(void)
{
    //inicializar i2c no modo mestre
    int i2c_master_port = I2C_NUM_0;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_GPIO,
        .scl_io_num = SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };

    i2c_param_config(i2c_master_port, &conf);

    return i2c_driver_install(i2c_master_port, conf.mode, 0, 0, 0);
}

void lcd_show_temp(float temperature){
    /*Mostrar as temperaturas medidas através do display LCD
    utilizando a biblioteca i2c-lcd*/

    lcd_put_cur(0, 0);
    lcd_send_string("Temperatura:");

    lcd_put_cur(1, 0);

    char temp_str[10];
    sprintf(temp_str, "%.2f", temperature);
    lcd_send_string(temp_str);
    lcd_send_data(223); //mandando o código do símbolo de grau (°)
    lcd_send_data('C');
}

void app_main(void)
{
    ESP_ERROR_CHECK(i2c_master_init());
    //ESP_LOGI("I2C", "I2C initialized successfully");

    lcd_init();
    lcd_clear();
    hcsr04_config();
    relay_config();
    leds_config();
    ds18b20_init(DS18B20_GPIO);

    gpio_set_level(TRIGGER_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    for(;;) {
        float temp = ds18b20_get_temp();
        if(temp < (-50) || temp > 120){
            ESP_LOGI("ds18b20", "Temperatura fora dos limites aceitáveis.");
        }
        else{
            lcd_show_temp(temp);
        }

        hcsr04_init();

        if(pump_active){
            /*Se a bomba d'água estiver ligada as leituras ocorrem a cada 1 segundo 
            para garantir maior precisão para desligar a bomba*/
            vTaskDelay(pdMS_TO_TICKS(1000)); 
        }
        else{
            vTaskDelay(pdMS_TO_TICKS(5000));
        }

    }
}
