#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
#define DIST_MAX 5.32 //distância entre o sensor e o fundo do reservatório
#define DIST_MIN 2.30 //distância mínima entre o sensor e a superfície da água

#define DS18B20_GPIO 12 //Sensor temperatura

/*Leds e Buzzer*/
#define LED_G1_GPIO GPIO_NUM_9
#define LED_G2_GPIO GPIO_NUM_8
#define LED_Y1_GPIO GPIO_NUM_7
#define LED_Y2_GPIO GPIO_NUM_6
#define LED_R1_GPIO GPIO_NUM_10
#define LED_R2_GPIO GPIO_NUM_3
//#define BUZZER_GPIO GPIO_NUM_2

/*Relé*/
#define RELAY_1_GPIO GPIO_NUM_20 //controla bomba d'água
#define RELAY_2_GPIO GPIO_NUM_21 //controla resistência

/*Display LCD*/
#define SDA_GPIO GPIO_NUM_4
#define SCL_GPIO GPIO_NUM_5
#define TEMP_DEFAULT 25

/*Botões para definir a temperatura*/
#define BUTTON_UP_GPIO GPIO_NUM_13
#define BUTTON_DOWN_GPIO GPIO_NUM_2

/*Variáveis auxiliares para a bomba d'água e resitência*/
float percentage = 0;
bool pump_active = false;
bool resist_active = false;
int count_level = 0;
int count_temp = 0;

int select_temp = TEMP_DEFAULT;
bool edit_mode = false;
TaskHandle_t edit_task_handle = NULL;
TaskHandle_t temp_task_handle = NULL;


void leds_config()
{
    /*Configuração dos leds e buzzer*/
    gpio_pad_select_gpio(LED_G1_GPIO);
    gpio_pad_select_gpio(LED_G2_GPIO);
    gpio_pad_select_gpio(LED_Y1_GPIO);
    gpio_pad_select_gpio(LED_Y2_GPIO);
    gpio_pad_select_gpio(LED_R1_GPIO);
    gpio_pad_select_gpio(LED_R2_GPIO);
    //gpio_pad_select_gpio(BUZZER_GPIO);
    
    gpio_set_direction(LED_G1_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_G2_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_Y1_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_Y2_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_R1_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_R2_GPIO, GPIO_MODE_OUTPUT);
    //gpio_set_direction(BUZZER_GPIO, GPIO_MODE_OUTPUT);
}

void led_buzzer_blink(){
    /*Função que faz o buzzer e o led vermelho piscar 
    indicando algum erro*/
    //gpio_set_level(BUZZER_GPIO, 1);
    gpio_set_level(LED_R2_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    //gpio_set_level(BUZZER_GPIO, 0);
    gpio_set_level(LED_R2_GPIO, 0);
}

void leds_update(float percentage){
    /*Atualiza os leds de acordo com a porcentagem medida pelo sensor*/
    gpio_set_level(LED_G1_GPIO, percentage >= 90 ? 1 : 0);
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

void hcsr04_config(){
    //Configura GPIOs do sensor hcsr04
    gpio_reset_pin(TRIGGER_GPIO);
    gpio_set_direction(TRIGGER_GPIO, GPIO_MODE_OUTPUT);
    gpio_reset_pin(ECHO_GPIO);
    gpio_set_direction(ECHO_GPIO, GPIO_MODE_INPUT);
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

void read_buttons(){
    if (gpio_get_level(BUTTON_UP_GPIO) == 0 && gpio_get_level(BUTTON_DOWN_GPIO) == 0) {
        //Os dois botões foram presisonados então o display entra em modo de edição
        edit_mode = !edit_mode;
    }
}

void buttons_config(){
    //Cofigurando botões
    gpio_pad_select_gpio(BUTTON_UP_GPIO);
    gpio_pad_select_gpio(BUTTON_DOWN_GPIO);

    gpio_set_direction(BUTTON_UP_GPIO, GPIO_MODE_INPUT);
    gpio_set_direction(BUTTON_DOWN_GPIO, GPIO_MODE_INPUT);

    gpio_pulldown_en(BUTTON_UP_GPIO);
    gpio_pullup_dis(BUTTON_UP_GPIO);
    gpio_set_intr_type(BUTTON_UP_GPIO, GPIO_INTR_ANYEDGE);

    gpio_pulldown_en(BUTTON_DOWN_GPIO);
    gpio_pullup_dis(BUTTON_DOWN_GPIO);
    gpio_set_intr_type(BUTTON_DOWN_GPIO, GPIO_INTR_ANYEDGE);

    //Configura interrupções
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_UP_GPIO, read_buttons, NULL);
    gpio_isr_handler_add(BUTTON_DOWN_GPIO, read_buttons, NULL);
}

void turn_pump_on() {
    /*Liga a bomba d'água*/
    gpio_set_level(RELAY_1_GPIO, 0);
    pump_active = true;
    printf("Bomba d'água ligada\n");
}

void turn_pump_off() {
    /*Desliga a bomba d'água*/
    gpio_set_level(RELAY_1_GPIO, 1);
    pump_active = false;
    printf("Bomba d'água desligada\n");
}

void turn_resist_on() {
    /*Liga a resistência*/
    gpio_set_level(RELAY_2_GPIO, 0);
    resist_active = true;
    printf("Resistência ligada\n");
}

void turn_resist_off() {
    /*Desliga a resistência*/
    gpio_set_level(RELAY_2_GPIO, 1);
    resist_active = false;
    printf("Resistência desligada\n");
}

static esp_err_t i2c_master_init(void)
{
    //Inicializaçãoc do i2c no modo mestre
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

void lcd_edit_temp(float temperature){
    /*Mostrar a edição de temperatura através do display LCD
    utilizando a biblioteca i2c-lcd*/
    
    lcd_put_cur(0, 0);
    lcd_send_string("Modo edicao");

    lcd_put_cur(1, 0);

    char temp_str[10];
    sprintf(temp_str, "%.2f", temperature);
    lcd_send_string(temp_str);
}

float read_temperature(){
    //Função que faz a leitura da temperatura

    float temp = ds18b20_get_temp();
    
    if(temp < 10 || temp > 50){
        ESP_LOGI("ds18b20", "Temperatura fora dos limites aceitáveis.");
    }
    else{
        if(temp < select_temp){
            //Se a temperatura atual estiver abaixo da temperatura indicada pelo usuário e o nível de água estiver acima de 10%
            if(percentage > 10){
                count_temp++;
            }
            if(count_temp == 3){
                /*Se houverem 3 leituras da temperatura da água menor que a indicada pelo usuário
                devemos ligar a resistência*/
                turn_resist_on();
            }
        }
        else{
            if(resist_active){
                /*Se a bomba estiver ligada mas a temperatura é maior
                devemos desligar a resistência*/
                turn_resist_off();
            }
            //Se a temperatura for maior que a estabelecida, zera o contador de leituras 
            count_temp = 0; 
        }
        
    }
    return temp;
}

void set_temperature(){
    /*Função que lê os botões e modifica a temperatura definida aumentando ou diminuindo 1°C*/

    int current_state_up = gpio_get_level(BUTTON_UP_GPIO);
    int current_state_down = gpio_get_level(BUTTON_DOWN_GPIO);
    if(current_state_up != 1){
        vTaskDelay(50 / portTICK_PERIOD_MS);
        if(current_state_up == gpio_get_level(BUTTON_UP_GPIO)){
            if(current_state_up == 0 && select_temp < 50){
                select_temp++;
            }
        }
    }
    if(current_state_down != 1){
        vTaskDelay(50 / portTICK_PERIOD_MS);
        if(current_state_down == gpio_get_level(BUTTON_DOWN_GPIO)){
            if(current_state_down == 0 && select_temp > 10){
                select_temp--;
            }
        }
    }
}

void edit_task(void *pvParameters){
    /*Tarefa referente a quando o sistema está em modo de edição, e o usuário pode escolher mudar a temperatura desejada*/

    while(1){
        set_temperature();
        lcd_edit_temp(select_temp);

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void temperature_task(void *pvParameters){
    /*Tarefa que administra as leituras de temperatura da água e exibe através do display*/

    lcd_init();
    lcd_clear();

    ds18b20_init(DS18B20_GPIO);

    while(1){
        float current_temp = read_temperature();
        lcd_show_temp(current_temp);

        if(resist_active){
            vTaskDelay(2000 / portTICK_PERIOD_MS);

        }
        else{
            vTaskDelay(10000 / portTICK_PERIOD_MS);
        }
        
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void read_level(){
    /*Função que faz a leitura do sensor de nível*/

    //Envia o sinal trigger
    gpio_set_level(TRIGGER_GPIO, 1);
    ets_delay_us(20);
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
    percentage = ((DIST_MAX - distance)/(DIST_MAX - DIST_MIN)) * 100; 

    if(percentage >= 0){ //Porcentagem válida
        if(percentage <= 90){
            count_level += 1;
            if(count_level == 5){ 
                /*Se houverem 5 leituras do nível da caixa menor que 90
                devemos ligar a bomba*/
                turn_pump_on();
            }
        }
        else{
            if(pump_active){
                /*Se a bomba estiver ligada mas a porcentagem é maior que 90
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

void level_task(void *pvParameters){
    /*Tarefa que administra as leituras do nível e exibe através dos LEDs*/

    hcsr04_config();
    leds_config();

    gpio_set_level(TRIGGER_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    for(;;){

        read_level();
        if(pump_active){
            /*Se a bomba d'água estiver ligada as leituras ocorrem a cada 2 segundos
            para garantir maior precisão para desligar a bomba*/
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
        else{
            vTaskDelay(10000 / portTICK_PERIOD_MS);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void control_task(void *pvParameters){
    /*Tarefa que cria e controla as tarefas sobre o modo de edição, onde o usuário modifica o parâmetro de temperatura,
    e o modo de execução, no qual o display exibe as informações sobre a temperatura atual da água*/
    
    xTaskCreate(&temperature_task, "temperature_task", 2048, NULL, 10, &temp_task_handle);
    xTaskCreate(&edit_task, "edit_task", 2048, NULL, 10, &edit_task_handle);

    while(1){
        if(edit_mode){
            vTaskSuspend(temp_task_handle);
            vTaskResume(edit_task_handle);
        }
        else{
            vTaskSuspend(edit_task_handle);
            vTaskResume(temp_task_handle);
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }

    
}

void app_main(void)
{
    ESP_ERROR_CHECK(i2c_master_init());
    
    buttons_config();
    relay_config();
    
    xTaskCreate(&level_task, "level_task", 2048, NULL, 5, NULL);
    xTaskCreatePinnedToCore(control_task, "control_task", 2048, NULL,10, NULL, 1);

}
