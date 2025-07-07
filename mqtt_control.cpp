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
// const std::string TOPIC_PUB_PH { "sensors/ph" };
// const std::string TOPIC_PUB_WATER_LEVEL { "sensors/water_level" };
// const std::string TOPIC_PUB_LIGHT { "sensors/light" };

const int QOS = 1;

// 센서 값 읽기
// 현재는 랜덤 값으로 넣어져 있음. 실제 GPIO 센서 값을 읽는 코드로 교체해야함.
float read_ph_sensor() {

    return (rand() % 140) / 10.0;
}

float read_water_level_sensor() {
    
    return (rand() % 1010) / 10.0;
}

float read_light_sensor() {
   
    return rand() % 1024;
}


// MQTT 이벤트 처리를 위한 콜백 클래스
class callback : public virtual mqtt::callback {
public:
    void connection_lost(const std::string& cause) override {
        std::cerr << "\n연결을 잃었습니다.." << std::endl;
        if (!cause.empty())
            std::cerr << "\t이유: " << cause << std::endl;
    }

    // 제어 메시지 수신 시 호출되는 함수
    void message_arrived(mqtt::const_message_ptr msg) override {
        std::cout << "Message arrived" << std::endl;
        std::cout << "\ttopic: '" << msg->get_topic() << "'" << std::endl;
        std::cout << "\tpayload: '" << msg->to_string() << "'\n" << std::endl;

        // "PUMP_ON" 메시지를 받으면 펌프용 GPIO를 HIGH로 설정하는 제어 명령에 따른 GPIO 동작 코드 추가해야함.
    }
};

int main(int argc, char* argv[]) {
    // GPIO 라이브러리 초기화
    if (gpioInitialise() < 0) {
        std::cerr << "pigpio 초기화 실패" << std::endl;
        return 1;
    }

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
        mqtt::token_ptr conntok = client.connect(connOpts);
        conntok->wait();
        std::cout << "Connected!" << std::endl;

        // 제어 명령 토픽 구독
        std::cout << "Subscribed to topic... '" << TOPIC_SUB_CONTROL << "'..." << std::endl;
        client.subscribe(TOPIC_SUB_CONTROL, QOS);
        std::cout << "구독 완료!" << std::endl;

        // 주기적으로 센서 데이터 발행
        while (true) {
            // 1. 센서 값 읽기
            float ph_value = read_ph_sensor();
            float water_level_value = read_water_level_sensor();
            float light_value = read_light_sensor();

            // 2. JSON 형식의 문자열 생성 (핵심 변경 부분)
            std::string payload = "{"
            "\"ph\": " + std::to_string(ph_value) + ","
            "\"water_level\": " + std::to_string(water_level_value) + ","
            "\"light\": " + std::to_string(light_value) +
            "}";

            // 3. 통합된 토픽으로 JSON 페이로드 발행
            client.publish(TOPIC_PUB_DATA, payload, QOS, false);

            std::cout << "Published to topic '" << TOPIC_PUB_DATA << "': " << payload << std::endl;

            // 10초 대기
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }
    catch (const mqtt::exception& exc) {
        std::cerr << "Error: " << exc.what() << std::endl;
        gpioTerminate(); // 프로그램 종료 전 GPIO 정리
        return 1;
    }
    
    gpioTerminate();
    return 0;
}
