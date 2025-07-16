#include <iostream>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>
#include <memory>
#include <pigpio.h> // GPIO 라이브러리
#include "mqtt/async_client.h" // Paho MQTT 라이브러리

// MQTT 브로커 정보
const std::string SERVER_ADDRESS { "tcp://localhost:1883" };
const std::string CLIENT_ID { "raspberrypi_client" };

// 구독 및 발행할 토픽 정보
const std::string TOPIC_SUB_CONTROL { "device/control" };
const std::string TOPIC_PUB_DATA { "farm/data/zone-A" }; // 발행 토픽을 하나로 통합

const int QOS = 1;

// BH1750 I2C 조도 센서
const int I2C_BUS = 1; // 라즈베리파이의 I2C 버스 번호 (보통 1번)
const int BH1750_ADDR = 0x23; // BH1750 센서의 기본 I2C 주소

// 수위 센서
const int PIN_WATER_LEVEL = 23; // 수위 센서가 연결된 GPIO 핀

// 릴레이 제어 핀 설정
const int PIN_LED_RELAY = 17; // 릴레이의 IN 핀에 연결된 GPIO 번호

// PH에서 사용할 ADC: MCP3008 ADC (SPI) -> 가정
const unsigned int SPI_CHANNEL = 0; // SPI 채널 0
const unsigned int SPI_SPEED = 50000; // SPI 통신 속도

int i2c_handle_light; // I2C 통신 핸들을 저장할 전역 변수
int spi_handle_adc; // SPI 핸들

// PH 센서 값 (현재는 랜덤 값으로 넣어져 있음. 실제 GPIO 센서 값을 읽는 코드로 교체해야함.)
float read_ph_sensor() {
    return (rand() % 140) / 10.0;
}

// 수위 센서 값(1.0, 0.0) 반환, 실패 시 음수 반환
float read_water_level_sensor() {
    int level = gpioRead(PIN_WATER_LEVEL);

    if (level < 0) {
        std::cerr << "Failed to read: Water GPIO pin: " << PIN_WATER_LEVEL << ". Error code: " << level << std::endl;
        return -1.0f;
    }

    return static_cast<float>(level);
}

// 측정된 조도(lux) 값 반환, 실패 시 음수 반환
float read_light_sensor() {
    // 고해상도 모드로 측정 시작 명령
    if (i2cWriteByte(i2c_handle_light, 0x10) < 0) {
        std::cerr << "Failed to write: BH1750 sensor." << std::endl;
        return -1.0f;
    }

    // 센서가 빛을 측정하고 값을 변환할 시간 (최대: 180ms)
    gpioSleep(PI_TIME_RELATIVE, 0, 180000);

    char data[2];
    // 측정 결과 읽기
    if (i2cReadDevice(i2c_handle_light, data, 2) == 2) {
        // 수신된 2바이트 데이터를 하나의 16비트 정수 값으로 변환
        int raw_value = (data[0] << 8) | data[1];
        
        // lux 값으로 변환
        float lux = raw_value / 1.2f;
        return lux;
    } else {
        std::cerr << "Failed to read: BH1750 sensor" << std::endl;
        return -1.0f;
    }
}


// MQTT 이벤트 처리를 위한 콜백 클래스
class callback : public virtual mqtt::callback {
public:
    void connection_lost(const std::string& cause) override {
        std::cerr << "\nConnection lost..." << std::endl;
        if (!cause.empty())
            std::cerr << "\tCause: " << cause << std::endl;
    }

    // 제어 메시지 수신 시 호출되는 함수
    void message_arrived(mqtt::const_message_ptr msg) override {
        std::cout << "Message arrived" << std::endl;
        std::cout << "\ttopic: '" << msg->get_topic() << "'" << std::endl;
        std::string payload = msg->to_string(); 
        std::cout << "\tpayload: '" << payload << "'\n" << std::endl;

        // 수신된 메시지에 따라 릴레이 제어
        if (payload == "LED_ON") { // LED 켜기기
            std::cout << "Turning LED ON..." << std::endl;
            gpioWrite(PIN_LED_RELAY, 0); 
        } else if (payload == "LED_OFF") { // LED 끄기기
            std::cout << "Turning LED OFF..." << std::endl;
            gpioWrite(PIN_LED_RELAY, 1);
        }

        // "PUMP_ON" 메시지를 받으면 펌프용 GPIO를 HIGH로 설정하는 제어 명령에 따른 GPIO 동작 코드 추가해야함.
    }
};

int main(int argc, char* argv[]) {
    // GPIO 라이브러리 초기화
    if (gpioInitialise() < 0) {
        std::cerr << "pigpio initialization failed." << std::endl;
        return 1;
    }

    // I2C 핸들 초기화
    // 프로그램 시작 시 I2C 버스 한 번만 열기.
    i2c_handle_light = i2cOpen(I2C_BUS, BH1750_ADDR, 0);
    if (i2c_handle_light < 0) {
        std::cerr << "Failed to open I2C. Check if the I2C interface is enabled." << std::endl;
        std::cerr << "($ sudo raspi-config -> Interface Options -> I2C -> Enable)" << std::endl;
        gpioTerminate();
        return 1;
    }

    // 수위 센서 GPIO 핀 모드 설정
    gpioSetMode(PIN_WATER_LEVEL, PI_INPUT);
    gpioSetPullUpDown(PIN_WATER_LEVEL, PI_PUD_UP);

    // 릴레이 핀 초기화
    gpioSetMode(PIN_LED_RELAY, PI_OUTPUT);
    // 프로그램 시작 시 릴레이 OFF로 안전하게 초기화
    gpioWrite(PIN_LED_RELAY, 1); 

    mqtt::async_client client(SERVER_ADDRESS, CLIENT_ID);
    callback cb;
    client.set_callback(cb);

    auto connOpts = mqtt::connect_options_builder()
        .clean_session()
        .will(mqtt::message("client/status", "LWT: Client disconnected", QOS))
        .finalize();

    try {
        // 브로커에 접속
        std::cout << "Connecting to MQTT broker..." << std::endl;
        client.connect(connOpts)->wait();
        std::cout << "Connected!" << std::endl;

        // 제어 명령 토픽 구독
        std::cout << "Subscribing to topic: '" << TOPIC_SUB_CONTROL << "'..." << std::endl;
        client.subscribe(TOPIC_SUB_CONTROL, QOS)->wait();
        std::cout << "Subscribed successfully!" << std::endl;

        // 주기적으로 센서 데이터 발행
        while (true) {
            float ph_value = read_ph_sensor();
            float water_level_value = read_water_level_sensor();
            float light_value = read_light_sensor(); 

            // 1. 테스트용 임계값 설정
            const float LIGHT_THRESHOLD_LOW = 200.0f; // 이 값보다 어두우면 LED를 켬
            const float LIGHT_THRESHOLD_HIGH = 300.0f; // 이 값보다 밝으면 LED를 끔

            // 2. 조도 값에 따른 자동 제어 명령 발행
            if (light_value >= 0) { // 센서 값 읽기가 성공했을 때만 실행
                if (light_value < LIGHT_THRESHOLD_LOW) {
                    // 조도가 200 미만이면 LED_ON 명령을 스스로에게 보냄
                    std::cout << "Test Logic: Light is too low. Publishing LED_ON command." << std::endl;
                    client.publish(TOPIC_SUB_CONTROL, "LED_ON", QOS, false);
                } else if (light_value > LIGHT_THRESHOLD_HIGH) {
                    // 조도가 500 초과면 LED_OFF 명령을 스스로에게 보냄
                    std::cout << "Test Logic: Light is bright enough. Publishing LED_OFF command." << std::endl;
                    client.publish(TOPIC_SUB_CONTROL, "LED_OFF", QOS, false);
                }
            }

            // JSON 형식의 문자열 생성
            std::string payload = "{"
            "\"ph\": " + std::to_string(ph_value) + ","
            "\"water_level\": " + std::to_string(water_level_value) + ","
            "\"light\": " + std::to_string(light_value) +
            "}";

            // JSON 페이로드 발행
            client.publish(TOPIC_PUB_DATA, payload, QOS, false);

            std::cout << "Published to topic '" << TOPIC_PUB_DATA << "': " << payload << std::endl;

            // 10초 대기
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }
    catch (const mqtt::exception& exc) {
        std::cerr << "Error: " << exc.what() << std::endl;
    }
    
    // 리소스 정리
    i2cClose(i2c_handle_light);
    gpioTerminate();
    return 0;
}