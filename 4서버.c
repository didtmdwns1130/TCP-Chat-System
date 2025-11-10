#include <stdio.h>      // 표준 입출력 함수 사용 (printf, scanf 등)
#include <stdlib.h>     // 표준 라이브러리 함수 사용 (exit, atoi 등)
#include <string.h>     // 문자열 처리 함수 사용 (strcpy, strcmp 등)
#include <unistd.h>     // 유닉스 표준 함수 사용 (close, read, write 등)
#include <pthread.h>    // POSIX 스레드 라이브러리 사용 (pthread_create 등)
#include <sys/socket.h> // 소켓 프로그래밍 함수 사용 (socket, bind, listen, accept 등)
#include <netinet/in.h> // 인터넷 주소 구조체 사용 (struct sockaddr_in 등)
#include <arpa/inet.h>  // IP 주소 변환 함수 사용 (inet_ntop 등)
#include <time.h>       // 시간 관련 함수 사용 (time, struct tm 등)
#include <signal.h>     // 시그널 처리 함수 사용 (signal 등)
#include <errno.h>      // 에러 번호 처리 (errno)
#include <mysql/mysql.h> // MySQL C API 헤더 파일 추가
#include <openssl/sha.h> // 비밀번호 해싱을 위한 OpenSSL SHA256 (필요시)

// DB 연결 정보
#define DB_HOST     "10.10.21.122"    // MySQL 서버 주소 (IP 또는 도메인)
#define DB_USER     "user1"           // MySQL 사용자명
#define DB_PASS     "Marin0806!"      // MySQL 비밀번호
#define DB_NAME     "chat_db"         // 사용할 데이터베이스 이름
#define DB_PORT     3306              // MySQL 포트 (기본값 3306)
#define DB_SOCKET   NULL              // 소켓 파일 (로컬 연결 시 사용, TCP는 NULL)
#define DB_CLIENT_FLAGS 0             // 추가 클라이언트 플래그 (일반적으로 0)

// MySQL 연결 핸들
MYSQL *conn; // 전역 MySQL 연결 객체

// 프로토콜 정의 (클라이언트와 서버가 완전히 동일해야 함)
#define DEFAULT_PORT 8080 // 클라이언트용. 서버에서는 직접 포트 번호 사용
#define MAX_CLIENTS 100   // 서버용
#define MAX_ROOMS 50      // 서버용
#define BUFFER_SIZE 1024      // 메시지 버퍼 크기 (채팅 메시지, 쿼리 등)
#define USERNAME_SIZE 50      // 사용자명/닉네임 최대 길이
#define PASSWORD_SIZE 100     // 비밀번호 최대 길이 (해싱 후 저장 포함)

// 메시지 타입 열거형 (클라이언트와 서버가 완전히 동일해야 함)
typedef enum {
    MSG_REGISTER = 1,
    MSG_REGISTER_SUCCESS,
    MSG_LOGIN,
    MSG_LOGIN_SUCCESS,
    MSG_LOGIN_FAIL,
    MSG_PUBLIC_CHAT,
    MSG_PRIVATE_CHAT,
    MSG_CREATE_ROOM,
    MSG_JOIN_ROOM,
    MSG_LEAVE_ROOM,
    MSG_ROOM_CHAT,
    MSG_LIST_USERS,
    MSG_LIST_ROOMS,
    MSG_USER_JOIN,
    MSG_USER_LEAVE,
    MSG_ERROR,
    MSG_DUPLICATE_IP,
    MSG_LOGOUT,
    MSG_DELETE_ROOM,
    MSG_USER_CHANGE,         // <-- 이 줄 추가: 사용자 정보 변경 요청
    MSG_USER_CHANGE_SUCCESS  // <-- 이 줄 추가: 사용자 정보 변경 성공 응답
} MessageType;

// 메시지 구조체 (클라이언트와 서버가 완전히 동일해야 함)
typedef struct {
    MessageType type;
    char username[USERNAME_SIZE]; // 메시지를 보내는 사용자명 (로그인된 사용자)
    char password[PASSWORD_SIZE]; // 비밀번호 (로그인/회원가입 시 사용, MSG_USER_CHANGE 시 '현재' 비밀번호)
    char target[USERNAME_SIZE];   // 귓속말 대상 또는 특정 방 이름/ID (문자열) (MSG_USER_CHANGE 요청 시 사용 안 함)
    char content[BUFFER_SIZE];    // 일반적인 메시지 내용 (MSG_USER_CHANGE 요청 시 사용 안 함)

    // 사용자 정보 변경을 위한 전용 필드 추가:
    char new_username[USERNAME_SIZE]; // 변경할 새 사용자명 (비어있으면 변경 안 함)
    char new_password[PASSWORD_SIZE]; // 변경할 새 비밀번호 (비어있으면 변경 안 함)
    
    int room_id;                  // 채팅방 ID (정수)
    time_t timestamp;             // 메시지 전송 시각
} Message;

// 클라이언트 구조체 변경: pk_id, nickname 추가 및 로그인 상태 추적
typedef struct {
    int socket; // 클라이언트와의 통신에 사용되는 소켓 디스크립터
    char username[USERNAME_SIZE]; // 현재 로그인한 사용자의 'id' (DB의 id 필드)
    char nickname[USERNAME_SIZE]; // 현재 로그인한 사용자의 'nickname' (DB의 nickname 필드)
    char ip_address[INET_ADDRSTRLEN];
    int room_id; // 현재 접속 중인 방 ID (DB의 chatroom.room_id와 매핑)
    int logged_in; // 로그인 여부 (0: 미로그인, 1: 로그인)
    int user_pk_id; // DB의 users.pk_id를 저장 (FK 연동 및 내부 식별자)
    pthread_t thread;
} Client;

// Room 구조체 변경: creator_pk_id와 room_type 추가
typedef struct {
    int id; // DB의 chatroom.room_id와 매핑
    char name[USERNAME_SIZE]; // DB의 chatroom.room_name과 매핑
    int active; // 방 활성화 여부 (서버 메모리 관리용)
    int room_type; // DB의 chatroom.room_type (예: 0=공개, 1=비공개)
    int creator_pk_id; // DB의 chatroom.creator_id와 매핑
} Room;

// 전역 변수
int server_socket; // 이 줄을 추가합니다.
Client clients[MAX_CLIENTS]; // 클라이언트 연결 정보를 저장하는 배열 (최대 MAX_CLIENTS명)
Room rooms[MAX_ROOMS];       // 채팅방 정보를 저장하는 배열 (최대 MAX_ROOMS개, DB와 동기화)
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER; // 클라이언트 배열 보호용 뮤텍스
pthread_mutex_t rooms_mutex = PTHREAD_MUTEX_INITIALIZER;   // 채팅방 배열 보호용 뮤텍스
int client_count = 0; // 현재 접속 중인 클라이언트 수
int room_count = 0; // 이 변수도 DB와 동기화 필요
int g_public_chat_room_id = -1; // 공개 채팅방의 room_id를 저장할 전역 변수

// 함수 선언
void *handle_client(void *arg); // 클라이언트별 스레드 함수
void send_message_to_client(int client_index, MessageType type, const char* content, const char* sender); // 특정 클라이언트에게 메시지 전송
void broadcast_message(MessageType type, const char* content, const char* sender, int exclude_client_idx, int room_id); // 여러 클라이언트에게 메시지 브로드캐스트
int get_client_index(int client_socket); // 소켓으로 클라이언트 인덱스 찾기
// int register_user(const char* username, const char* password); // DB 연동으로 변경 예정
// int authenticate_user(const char* username, const char* password); // DB 연동으로 변경 예정
void create_room(int client_index, const char* room_name); // 채팅방 생성 함수 프로토타입
void join_room(int client_index, int room_id); // 채팅방 입장 함수 프로토타입
void leave_room(int client_index); // 채팅방 퇴장 함수 프로토타입
void list_users(int client_index); // 사용자 목록 전송 함수 프로토타입
void list_rooms(int client_index); // 채팅방 목록 전송 함수 프로토타입
void signal_handler(int sig); // 시그널 핸들러 함수 프로토타입
void cleanup_server(); // 서버 정리 함수 프로토타입

// DB 관련 새로운 함수 선언
void db_connect(); // MySQL 데이터베이스 연결 함수
void db_disconnect(); // MySQL 데이터베이스 연결 해제 함수
void hash_password_sha256(const char* password, char* outputBuffer); // 비밀번호를 SHA256으로 해싱하는 함수
int db_register_user(const char* username, const char* password_hash, const char* nickname, int* pk_id); // 프로토타입 확인
int db_authenticate_user(const char* username, const char* password_hash, int* pk_id, char* nickname_out); // 프로토타입 확인
int db_update_user_info(int user_pk_id, const char* new_username, const char* new_password_hash);
int db_create_room(const char* room_name, int room_type, int creator_pk_id, int* room_id_out); // 새 채팅방 생성(DB)
int db_join_chatroom_user(int room_id, int user_pk_id); // 채팅방 입장(DB)
int db_leave_chatroom_user(int room_id, int user_pk_id); // 채팅방 퇴장(DB)
int db_insert_chat_message(int room_id, int sender_pk_id, const char* message_content); // 채팅 메시지 저장(DB)
int find_client_by_username(const char* username); //
void remove_client(int client_index); //

int main(int argc, char* argv[]) {
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    int port;
    if(argc!=2) {                                   // 포트번호 인자 체크
		printf("Usage : %s <port>\n", argv[0]);
		exit(1);
	}
    // 강력한 명령행 인자 검증 - 포트 번호 필수!
    printf("명령행 인자 개수: %d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("인자 %d: %s\n", i, argv[i]);
    }
    
    if (argc != 2) {
        printf("\n오류: 포트 번호가 누락되었습니다!\n");
        printf("═══════════════════════════════════════════════════════════════════\n");
        printf("서버 실행 방법이 잘못되었습니다!\n");
        printf("═══════════════════════════════════════════════════════════════════\n");
        printf("올바른 사용법: %s <포트번호>\n", argv[0]);
        printf("예시: %s 8080\n", argv[0]);
        printf("예시: %s 9999\n", argv[0]);
        printf("═══════════════════════════════════════════════════════════════════\n");
        printf("설명:\n");
        printf("   - 포트 번호는 반드시 입력해야 합니다\n");
        printf("   - 권장 포트 범위: 1024-65535\n");
        printf("   - 1024 미만 포트는 관리자 권한이 필요할 수 있습니다\n");
        printf("═══════════════════════════════════════════════════════════════════\n");
        exit(EXIT_FAILURE);
    }
    
    // 포트 번호 검증
    if (argv[1] == NULL || strlen(argv[1]) == 0) { // 인자가 비어있는지 확인
        printf("포트 번호가 비어있습니다!\n"); // 포트 번호 인자가 비어있을 때 오류 메시지 출력
        exit(EXIT_FAILURE); // 프로그램 비정상 종료
    }
    
    port = atoi(argv[1]); // 문자열을 정수로 변환
    
    // 포트 번호가 0인 경우 (숫자가 아닌 문자열을 입력한 경우)
    if (port == 0 && strcmp(argv[1], "0") != 0) { // "0"이 아닌데 변환값이 0이면 잘못된 입력
        printf("잘못된 포트 번호입니다: '%s'\n", argv[1]); // 입력값이 숫자가 아닐 때 오류 메시지 출력
        printf("포트 번호는 숫자여야 합니다. 예: 8080, 9999\n"); // 올바른 입력 예시 안내
        exit(EXIT_FAILURE); // 프로그램 비정상 종료
    }
    
    if (port <= 0 || port > 65535) { // 포트 범위 체크 (1~65535만 허용)
        printf("포트 번호가 유효하지 않습니다: %d\n", port); // 잘못된 포트 번호 안내
        printf("포트 범위: 1-65535 (권장: 1024-65535)\n"); // 허용/권장 범위 안내
        exit(EXIT_FAILURE); // 잘못된 경우 프로그램 종료
    }
    
    if (port < 1024) { // 1024 미만 포트는 권장하지 않음
        printf("경고: 1024 미만 포트는 관리자 권한이 필요할 수 있습니다.\n"); // 1024 미만 포트 사용 시 경고 출력
        printf("포트 %d를 사용합니다...\n", port); // 실제 사용할 포트 번호 출력
    }
    
    printf("포트 번호 확인됨: %d\n", port); // 포트 정상 확인 메시지
    
    // 시그널 핸들러 설정
    signal(SIGINT, signal_handler);   // Ctrl+C 종료 시 처리
    signal(SIGTERM, signal_handler);  // kill 명령 등 종료 시 처리
    signal(SIGPIPE, SIG_IGN);         // 파이프 에러 무시
    
    // 클라이언트 배열 초기화
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].socket = -1;   // 소켓 비활성화
        clients[i].logged_in = 0; // 로그인 상태 초기화
        clients[i].room_id = 0;   // 방 정보 초기화
    }
    
    // 방 배열 초기화
    for (int i = 0; i < MAX_ROOMS; i++) {
        rooms[i].active = 0;      // 방 비활성화
        rooms[i].id = i + 1;      // 방 ID 부여 (1부터 시작)
    }
    
    // 서버 소켓 생성
    server_socket = socket(AF_INET, SOCK_STREAM, 0); // TCP 소켓 생성
    if (server_socket < 0) { // 소켓 생성에 실패한 경우
        perror("소켓 생성 실패"); // 에러 메시지 출력
        exit(EXIT_FAILURE);  // 프로그램 비정상 종료
    }
    
    // 소켓 재사용 옵션 설정
    int opt = 1; // 소켓 옵션 값 (1: 활성화)
    // SO_REUSEADDR 옵션을 설정하여 서버 재시작 시 "Address already in use" 오류 방지
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("소켓 옵션 설정 실패"); // 옵션 설정 실패 시 에러 출력
    }
    
    // 서버 주소 설정
    memset(&server_addr, 0, sizeof(server_addr)); // 구조체 초기화
    server_addr.sin_family = AF_INET;             // IPv4 사용
    server_addr.sin_addr.s_addr = INADDR_ANY;     // 모든 IP에서 접속 허용
    server_addr.sin_port = htons(atoi(argv[1]));  // 포트 설정 (네트워크 바이트 오더)
    
    // 바인드
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) { // 서버 소켓을 지정한 주소와 포트에 바인드(연결) 시도
        perror("바인드 실패"); // 바인드 실패 시 에러 메시지 출력
        perror("바인드 실패");
        printf("포트 %d가 이미 사용 중일 수 있습니다. 다른 포트를 시도해보세요.\n", port); // 포트 중복 사용 안내 메시지
        close(server_socket); // 소켓 닫기
        exit(EXIT_FAILURE);   // 프로그램 비정상 종료
    }
    
    // 리슨
    if (listen(server_socket, MAX_CLIENTS) < 0) { // 클라이언트 대기열 설정
        perror("리슨 실패");              // listen() 함수 실패 시 에러 메시지 출력
        close(server_socket);             // 서버 소켓 닫기 (자원 해제)
        exit(EXIT_FAILURE);               // 프로그램 비정상 종료
    }
    
    // DB 연결 시도 (서버 시작 시)
    db_connect();

    if (db_load_public_chat_room_id() != 0) {
        fprintf(stderr, "[%s]서버 시작 중 공개 채팅방 ID 로드에 실패했습니다. 종료합니다.\n", __TIME__);
        // g_public_chat_room_id가 -1이면 공개채팅 불가하므로 서버 종료 고려
        cleanup_server(); // 정리 함수 호출
        return 1;
    }

    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("                        채팅 서버 시작!   \n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("서버가 실행 중입니다.\n");
    printf("클라이언트 접속 대기 중...\n");
    printf("IP 중복 접속 방지 기능 활성화\n");
    printf("서버 종료: Ctrl+C\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");
    
    // 클라이언트 접속 처리
    while (1) { // 무한 루프: 클라이언트 접속을 계속 대기
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len); // 클라이언트 접속 수락
        if (client_socket < 0) { // 접속 실패 시
            if (errno == EINTR) continue; // 시그널로 인한 중단은 무시하고 계속 대기
            perror("클라이언트 접속 실패"); // 그 외의 에러는 에러 메시지 출력
            continue; // 다음 접속 대기
        }
        
        char client_ip[INET_ADDRSTRLEN]; // 클라이언트 IP 주소를 저장할 버퍼
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN); // 네트워크 주소를 문자열(IP)로 변환
        
        printf("[%s] 새로운 접속 시도: %s\n",  // 새 클라이언트가 접속을 시도할 때 IP와 함께 로그 출력
               __TIME__, client_ip); // 새 클라이언트가 접속을 시도할 때 IP와 함께 로그 출력
        
        // IP 중복 체크 (더 엄격한 검사)
        pthread_mutex_lock(&clients_mutex);
        int ip_duplicate = 0;
        int connected_count = 0;
        
        // 현재 연결된 모든 클라이언트 확인
        for (int i = 0; i < MAX_CLIENTS; i++) { // 모든 클라이언트 슬롯 순회
            if (clients[i].socket != -1) { // 해당 슬롯이 활성(연결됨) 상태인지 확인
                connected_count++; // 연결된 클라이언트 수 증가
                printf("[DEBUG] 기존 연결 #%d: IP=%s, 로그인=%s\n", 
                       i, clients[i].ip_address, 
                       clients[i].logged_in ? "예" : "아니오"); // 현재 슬롯의 정보 출력
                
                if (strcmp(clients[i].ip_address, client_ip) == 0) { // IP 주소가 중복되는지 확인
                    ip_duplicate = 1; // 중복 플래그 설정
                    printf("[DEBUG] IP 중복 감지: 슬롯 #%d에서 같은 IP (%s) 발견\n", i, client_ip); // 중복 감지 로그 출력
                    break; // 더 이상 검사하지 않고 반복문 종료
                }
            }
        }
        pthread_mutex_unlock(&clients_mutex); // 클라이언트 배열 접근 뮤텍스 해제
        
        printf("[DEBUG] 현재 연결된 클라이언트 수: %d, IP 중복: %s\n", 
               connected_count, ip_duplicate ? "예" : "아니오"); // 현재 연결 수와 IP 중복 여부 출력
        
        if (ip_duplicate) { // 만약 IP가 중복된다면
            printf("[%s] IP 중복 접속 차단: %s\n", __TIME__, client_ip); // 로그 출력
            
            Message reject_msg; // 거절 메시지 구조체 선언
            memset(&reject_msg, 0, sizeof(Message)); // 구조체 초기화
            reject_msg.type = MSG_DUPLICATE_IP; // 메시지 타입 설정
            strcpy(reject_msg.content, "같은 IP에서 이미 접속 중입니다. 중복 접속은 허용되지 않습니다."); // 안내 메시지
            
            send(client_socket, &reject_msg, sizeof(Message), 0); // 클라이언트에게 거절 메시지 전송
            close(client_socket); // 소켓 닫기
            continue; // 다음 클라이언트 대기 루프로 이동
        }
        
        // 빈 클라이언트 슬롯 찾기
        pthread_mutex_lock(&clients_mutex); // 클라이언트 배열 접근을 위해 뮤텍스 잠금
        int client_index = -1; // 빈 슬롯 인덱스를 저장할 변수 초기화
        for (int i = 0; i < MAX_CLIENTS; i++) { // 모든 클라이언트 슬롯 순회
            if (clients[i].socket == -1) { // 비어있는(미사용) 슬롯 찾기
            client_index = i; // 빈 슬롯 인덱스 저장
            clients[i].socket = client_socket; // 해당 슬롯에 새 클라이언트 소켓 할당
            strcpy(clients[i].ip_address, client_ip); // 클라이언트 IP 저장
            clients[i].logged_in = 0; // 로그인 상태 초기화(미로그인)
            clients[i].room_id = 0; // 방 정보 초기화(공개 채팅방)
                break;
            }
        }
        pthread_mutex_unlock(&clients_mutex); // 클라이언트 배열 접근 뮤텍스 해제
        
        if (client_index == -1) { // 빈 클라이언트 슬롯이 없는 경우 (최대 접속자 초과)
            printf("[%s] 최대 클라이언트 수 초과: %s\n", __TIME__, client_ip); // 초과 안내 로그 출력
            
            Message reject_msg;
            memset(&reject_msg, 0, sizeof(Message)); // 거절 메시지 구조체 초기화
            reject_msg.type = MSG_ERROR; // 메시지 타입 설정
            strcpy(reject_msg.content, "서버가 가득 찼습니다. 나중에 다시 시도해주세요."); // 안내 메시지 설정
            
            send(client_socket, &reject_msg, sizeof(Message), 0); // 클라이언트에게 거절 메시지 전송
            close(client_socket); // 소켓 닫기
            continue; // 다음 클라이언트 대기 루프로 이동
        }
        
        // 클라이언트 핸들러 스레드 생성
        int *client_idx = malloc(sizeof(int)); // 클라이언트 인덱스를 위한 동적 메모리 할당
        *client_idx = client_index; // 인덱스 값 저장
        
        // 클라이언트별 스레드 생성 (handle_client 함수에 인덱스 전달)
        if (pthread_create(&clients[client_index].thread, NULL, handle_client, client_idx) != 0) {
            perror("스레드 생성 실패"); // 스레드 생성 실패 시 에러 출력
            free(client_idx); // 할당한 메모리 해제
            pthread_mutex_lock(&clients_mutex);
            clients[client_index].socket = -1; // 클라이언트 슬롯 비활성화
            pthread_mutex_unlock(&clients_mutex);
            close(client_socket); // 소켓 닫기
            continue;
        }
        
        pthread_detach(clients[client_index].thread); // 스레드 자원 자동 회수(좀비 스레드 방지)
        printf("[%s] 클라이언트 연결 완료: %s (슬롯 %d)\n", 
               __TIME__, client_ip, client_index); // 접속 성공 로그 출력
    }
    
    cleanup_server(); // 서버에서 사용한 리소스를 정리하고 종료 준비를 합니다.
    return 0;         // 프로그램을 정상적으로 종료함을 운영체제에 알립니다.
}

void *handle_client(void *arg) { // 클라이언트별 통신을 처리하는 스레드 함수 시작
    int client_index = *(int*)arg; // 전달받은 인덱스 포인터를 정수형 인덱스로 변환
    free(arg); // 동적으로 할당된 인덱스 메모리 해제
    
    Message msg; // 클라이언트로부터 수신할 메시지 구조체 선언
    
    
    while (1) {
        int bytes_received = recv(clients[client_index].socket, &msg, sizeof(Message), 0);
        if (bytes_received <= 0) {
            printf("[%s] 클라이언트 연결 종료: %s (로그인 ID: %s, 닉네임: %s)\n", 
                   __TIME__, clients[client_index].ip_address, 
                   strlen(clients[client_index].username) > 0 ? clients[client_index].username : "미로그인",
                   strlen(clients[client_index].nickname) > 0 ? clients[client_index].nickname : "없음");
            
            // 만약 로그아웃되지 않은 상태에서 연결이 끊겼다면, DB에서 강제로 상태 업데이트 (예: 접속 상태 변경)
            // (이 부분은 필요에 따라 추가 구현)

            // 방에 있었다면 방에서 나가도록 처리
            if (clients[client_index].room_id > 0) {
                // 특정 방에 대한 퇴장 메시지 (room_id가 0이 아니면)
                broadcast_message(MSG_ROOM_CHAT, "퇴장", clients[client_index].nickname, client_index, clients[client_index].room_id);
            } else if (clients[client_index].logged_in) {
                // 공개 채팅방에 대한 퇴장 메시지 (로그인 상태인 경우)
                broadcast_message(MSG_PUBLIC_CHAT, "퇴장", clients[client_index].nickname, client_index, g_public_chat_room_id);
            }

            // 클라이언트 구조체에서 제거
            remove_client(client_index);
            break;
        }
        
        switch (msg.type) {
            case MSG_REGISTER: { // 회원가입 요청 처리
                printf("[%s] 새로운 회원가입 요청: ID=%s, 닉네임=%s\n", __TIME__, msg.username, msg.target); // 닉네임 로그 출력 추가
                char hashed_password[SHA256_DIGEST_LENGTH * 2 + 1];
                hash_password_sha256(msg.password, hashed_password);
                int new_user_pk_id;

                // db_register_user 함수 호출 시 msg.target (닉네임) 전달
                int result = db_register_user(msg.username, hashed_password, msg.target, &new_user_pk_id); 
                
                if (result == 0) {
                    send_message_to_client(client_index, MSG_REGISTER_SUCCESS, "✅ 회원가입 성공! 이제 로그인할 수 있습니다.", "서버");
                    printf("[%s] 회원가입 성공: ID=%s (PK: %d, 닉네임: %s)\n", __TIME__, msg.username, new_user_pk_id, msg.target); // 닉네임 로그 출력 추가
                } else if (result == -2) {
                    send_message_to_client(client_index, MSG_ERROR, "❌ 회원가입 실패: 이미 존재하는 ID입니다.", "서버");
                    printf("[%s] 회원가입 실패: 중복 ID (%s)\n", __TIME__, msg.username);
                } else {
                    send_message_to_client(client_index, MSG_ERROR, "❌ 회원가입 중 오류가 발생했습니다.", "서버");
                    printf("[%s] 회원가입 실패: DB 오류 for ID (%s)\n", __TIME__, msg.username);
                }
                break;
            }
            case MSG_LOGIN: { // MSG_LOGIN 메시지 타입 처리 시작 (로그인 요청)
                printf("[%s] 로그인 요청: %s (IP: %s)\n", __TIME__, msg.username, clients[client_index].ip_address); // 로그인 요청 로그 출력

                // 이미 로그인된 사용자인지 확인
                pthread_mutex_lock(&clients_mutex); // 클라이언트 배열 접근을 위해 뮤텍스 잠금
                for (int i = 0; i < MAX_CLIENTS; i++) { // 모든 클라이언트 슬롯 순회
                if (clients[i].socket != -1 && clients[i].logged_in &&  // 해당 슬롯이 활성화되어 있고 로그인 상태이며
                strcmp(clients[i].username, msg.username) == 0 && i != client_index) { // 사용자명이 동일하고 자기 자신이 아닌 경우
                send_message_to_client(client_index, MSG_ERROR,  // 에러 메시지 전송 (이미 로그인된 사용자)
                "이미 로그인된 사용자입니다.", "서버");
                pthread_mutex_unlock(&clients_mutex); // 뮤텍스 해제
                printf("[%s] 로그인 실패: %s (이미 로그인된 사용자)\n", __TIME__, msg.username); // 로그인 실패 로그 출력
                goto end_login_case; // 이 클라이언트의 로그인 처리 종료 (switch-case 밖 레이블로 이동)
                }
                }
                // 같은 IP에서 다른 계정으로 로그인한 경우도 방지
                for (int i = 0; i < MAX_CLIENTS; i++) { // 모든 클라이언트 슬롯 순회
                if (clients[i].socket != -1 && clients[i].logged_in && // 해당 슬롯이 활성화되어 있고 로그인 상태이며
                strcmp(clients[i].ip_address, clients[client_index].ip_address) == 0 && i != client_index) { // IP가 동일하고 자기 자신이 아닌 경우
                send_message_to_client(client_index, MSG_DUPLICATE_IP,  // 중복 IP 에러 메시지 전송
                "해당 IP로 다른 계정이 이미 로그인되어 있습니다.", "서버");
                pthread_mutex_unlock(&clients_mutex); // 뮤텍스 해제
                printf("[%s] 로그인 실패: %s (동일 IP 로그인 중복)\n", __TIME__, msg.username); // 로그인 실패 로그 출력
                goto end_login_case; // 이 클라이언트의 로그인 처리 종료
                }
                }
                pthread_mutex_unlock(&clients_mutex); // 클라이언트 배열에 대한 뮤텍스 잠금 해제

                char hashed_password[SHA256_DIGEST_LENGTH * 2 + 1]; // 해싱된 비밀번호를 저장할 버퍼 선언
                hash_password_sha256(msg.password, hashed_password); // 입력받은 비밀번호를 SHA256으로 해싱

                int user_pk_id = -1; // 인증된 사용자의 pk_id를 저장할 변수 초기화
                char user_nickname[USERNAME_SIZE]; // 인증된 사용자의 닉네임을 저장할 버퍼 선언

                // db_authenticate_user 함수 호출하여 DB에서 사용자명과 해싱된 비밀번호로 인증 시도
                int result = db_authenticate_user(msg.username, hashed_password, &user_pk_id, user_nickname);

                if (result == 1) { // 인증 성공 시
                pthread_mutex_lock(&clients_mutex); // 클라이언트 배열에 대한 뮤텍스 잠금
                clients[client_index].logged_in = 1; // 해당 클라이언트의 로그인 상태를 1(로그인)로 설정
                strcpy(clients[client_index].username, msg.username); // 클라이언트 구조체에 사용자명 저장
                strcpy(clients[client_index].nickname, user_nickname); // 클라이언트 구조체에 닉네임 저장
                clients[client_index].user_pk_id = user_pk_id; // 클라이언트 구조체에 pk_id 저장
                pthread_mutex_unlock(&clients_mutex); // 클라이언트 배열에 대한 뮤텍스 잠금 해제
                
                send_message_to_client(client_index, MSG_LOGIN_SUCCESS, 
                    "로그인 성공!", clients[client_index].nickname); // 로그인 성공 메시지를 닉네임으로 전송
                // 모든 클라이언트에게 새 사용자 접속 알림 (닉네임 사용)
                char join_msg[BUFFER_SIZE]; // 접속 알림 메시지를 저장할 버퍼 선언
                snprintf(join_msg, sizeof(join_msg), "🗣️  %s님이 접속했습니다.", clients[client_index].nickname); // 접속한 사용자의 닉네임을 포함한 메시지 생성
                broadcast_message(MSG_USER_JOIN, join_msg, "서버", client_index, 0); // 공개 채팅방(0)에 접속 알림 메시지를 브로드캐스트(본인 제외)
                
                printf("[%s] 로그인 성공: %s (PK: %d, Nickname: %s, IP: %s)\n", 
                       __TIME__, msg.username, user_pk_id, user_nickname, clients[client_index].ip_address); // 로그인 성공 정보를 서버 콘솔에 출력
                } else {
                send_message_to_client(client_index, MSG_LOGIN_FAIL, 
                    "잘못된 사용자명 또는 비밀번호입니다.", "서버"); // 로그인 실패 시 클라이언트에 실패 메시지 전송
                printf("[%s] 로그인 실패: %s (잘못된 인증 정보)\n", __TIME__, msg.username); // 로그인 실패 로그 출력
                }
                end_login_case:; // goto 문을 위한 레이블(로그인 처리 종료)
                break;
            }
            case MSG_USER_CHANGE: {
                if (!clients[client_index].logged_in) { // 로그인 여부 확인
                    send_message_to_client(client_index, MSG_ERROR, "❌ 로그인 후 정보를 변경할 수 있습니다.", "서버");
                    break;
                }

                printf("[%s] 🔄 사용자 정보 변경 요청: %s (PK: %d)\n",
                       __TIME__, clients[client_index].username, clients[client_index].user_pk_id);

                char current_password_hashed[SHA256_DIGEST_LENGTH * 2 + 1];
                hash_password_sha256(msg.password, current_password_hashed); // 클라이언트가 보낸 현재 비밀번호 해싱

                // 1. 현재 비밀번호 인증
                int authenticated_pk_id = -1;
                char temp_nickname[USERNAME_SIZE]; 
                int auth_result = db_authenticate_user(clients[client_index].username, current_password_hashed, &authenticated_pk_id, temp_nickname);

                if (auth_result != 1 || authenticated_pk_id != clients[client_index].user_pk_id) {
                    send_message_to_client(client_index, MSG_ERROR, "❌ 현재 비밀번호가 일치하지 않습니다.", "서버");
                    printf("[%s] ❌ 정보 변경 실패: %s (현재 비밀번호 불일치)\n", __TIME__, clients[client_index].username);
                    break;
                }

                // 2. 변경할 새 정보 준비
                // msg.new_username에 새 사용자명이, msg.new_password에 새 비밀번호가 들어있습니다.
                const char* new_username = (strlen(msg.new_username) > 0) ? msg.new_username : NULL;
                char new_password_hashed[SHA256_DIGEST_LENGTH * 2 + 1];
                const char* password_to_update = NULL;

                if (strlen(msg.new_password) > 0) { // 새 비밀번호가 있다면 해싱
                    hash_password_sha256(msg.new_password, new_password_hashed);
                    password_to_update = new_password_hashed;
                }

                // 변경할 정보가 전혀 없는 경우를 방지 (클라이언트에서 미리 걸러지지만 서버에서도 확인)
                if (new_username == NULL && password_to_update == NULL) {
                    send_message_to_client(client_index, MSG_ERROR, "❌ 변경할 사용자명 또는 비밀번호를 입력해주세요.", "서버");
                    break;
                }

                // 3. DB 업데이트 호출
                int update_result = db_update_user_info(clients[client_index].user_pk_id, new_username, password_to_update);

                if (update_result == 0) { // 성공
                    // 서버 메모리의 클라이언트 정보 업데이트 (사용자명, 닉네임)
                    pthread_mutex_lock(&clients_mutex);
                    if (new_username != NULL) {
                        strncpy(clients[client_index].username, new_username, USERNAME_SIZE - 1);
                        clients[client_index].username[USERNAME_SIZE - 1] = '\0';
                        strncpy(clients[client_index].nickname, new_username, USERNAME_SIZE - 1); // 닉네임도 ID와 동일하게 업데이트
                        clients[client_index].nickname[USERNAME_SIZE - 1] = '\0';
                    }
                    pthread_mutex_unlock(&clients_mutex);

                    // 클라이언트에게 성공 메시지 전송
                    Message response_msg;
                    memset(&response_msg, 0, sizeof(Message));
                    response_msg.type = MSG_USER_CHANGE_SUCCESS;
                    strncpy(response_msg.content, "정보가 성공적으로 변경되었습니다.", BUFFER_SIZE - 1);
                    response_msg.content[BUFFER_SIZE - 1] = '\0';
                    if (new_username != NULL) { // 변경된 새 사용자명도 같이 보냄 (target 필드 활용)
                        strncpy(response_msg.target, new_username, USERNAME_SIZE - 1);
                        response_msg.target[USERNAME_SIZE - 1] = '\0';
                    }
                    send(clients[client_index].socket, &response_msg, sizeof(Message), 0);

                    printf("[%s] ✅ 사용자 정보 변경 성공: %s (ID: %s, PW: %s)\n", __TIME__,
                           clients[client_index].nickname,
                           new_username ? new_username : "(동일)",
                           password_to_update ? "변경됨" : "(동일)");
                } else if (update_result == -2) { // 새 사용자명 중복
                    send_message_to_client(client_index, MSG_ERROR, "❌ 변경하려는 사용자명이 이미 존재합니다.", "서버");
                    printf("[%s] ❌ 정보 변경 실패: %s (새 사용자명 중복)\n", __TIME__, new_username);
                } else { // DB 오류
                    send_message_to_client(client_index, MSG_ERROR, "❌ 정보 변경에 실패했습니다. (DB 오류)", "서버");
                    printf("[%s] ❌ 정보 변경 실패: %s (DB 오류)\n", __TIME__, clients[client_index].username);
                }
                break;
            }            
            case MSG_PUBLIC_CHAT:
                if (clients[client_index].logged_in) { // 로그인된 사용자만 공개 채팅 가능
                    printf("[%s] [공개채팅] %s: %s\n", __TIME__, clients[client_index].nickname, msg.content); // 공개채팅 메시지를 서버 콘솔에 출력
                    
                    // g_public_chat_room_id가 유효한지 확인 후 사용
                    if (g_public_chat_room_id != -1) { // 공개 채팅방 ID가 유효하면
                        db_insert_chat_message(g_public_chat_room_id, clients[client_index].user_pk_id, msg.content); // DB에 메시지 저장
                    } else {
                        fprintf(stderr, "[%s] 공개 채팅방 ID가 유효하지 않아 메시지를 DB에 저장하지 못했습니다.\n", __TIME__); // 공개 채팅방 ID가 없으면 에러 출력
                    }
                    
                    broadcast_message(MSG_PUBLIC_CHAT, msg.content,
                        clients[client_index].nickname, client_index, 0); // 공개 채팅방에 메시지 브로드캐스트(본인 제외, 닉네임 사용)
                }
                break;
                
            case MSG_PRIVATE_CHAT: {
            printf("[%s] 📩 귓속말 수신 (보낸이: %s, 대상: '%s', 내용: %s)\n", // 대상 닉네임에 쿼트 추가
                   __TIME__, clients[client_index].nickname, msg.target, msg.content);

            if (!clients[client_index].logged_in) {
                send_message_to_client(client_index, MSG_ERROR, "로그인 후 귓속말을 보낼 수 있습니다.", "서버");
                break;
            }

            // 대상 사용자가 로그인 중인지 확인
            int target_client_index = -1;
            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                // 디버깅을 위한 출력: 어떤 닉네임과 비교하는지 확인
                // 이 줄은 테스트 후 제거하셔도 됩니다.
                if (clients[i].socket != -1 && clients[i].logged_in) {
                    printf("  └> 비교 중: 온라인 사용자 '%s' (socket: %d) vs 대상 '%s'\n", clients[i].username, clients[i].socket, msg.target);
                }

                if (clients[i].socket != -1 && clients[i].logged_in &&
                    strcasecmp(clients[i].username, msg.target) == 0) { // <-- strcmp 대신 strcasecmp 사용
                    target_client_index = i;
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);

            if (target_client_index != -1) {
                // 귓속말 대상에게 메시지 전송
                char private_msg_content[BUFFER_SIZE + USERNAME_SIZE + 20];
                snprintf(private_msg_content, sizeof(private_msg_content),
                         "[귓속말 from]: %s", clients[client_index].username, msg.content);
                send_message_to_client(target_client_index, MSG_PRIVATE_CHAT, private_msg_content, clients[client_index].nickname);
                printf("[%s] 귓속말 전송 완료 (보낸이: %s -> 대상: %s)\n",
                       __TIME__, clients[client_index].nickname, msg.target);
                // 보낸이에게 성공 메시지 전송 (선택 사항)
                send_message_to_client(client_index, MSG_PRIVATE_CHAT, "귓속말을 성공적으로 전송했습니다.", "서버");
            } else {
                send_message_to_client(client_index, MSG_ERROR, "해당 사용자를 찾을 수 없거나 오프라인입니다.", "서버");
                printf("[%s] ❌ 귓속말 전송 실패: 대상 '%s' 찾을 수 없음\n", __TIME__, msg.target);
            }
            break;
        }
            
            case MSG_CREATE_ROOM: // MSG_CREATE_ROOM 메시지 타입 처리 시작 (채팅방 생성)
                if (clients[client_index].logged_in) { // 로그인한 사용자만 채팅방 생성 가능
                    printf("[%s] 채팅방 생성 요청: %s (by %s)\n", __TIME__, msg.target, clients[client_index].nickname); // 생성 요청 로그 출력
                    int new_room_id; // 새로 생성될 방의 ID 저장 변수
                    
                    // ⭐ 수정: room_type을 0 (공개)에서 1 (그룹/개별)로 변경 // 그룹/개별 방으로 생성
                    int result = db_create_room(msg.target, 1, clients[client_index].user_pk_id, &new_room_id); // DB에 방 생성 요청

                    if (result == 0) { // 성공 // DB에 방 생성 성공 시
                        // DB에 성공적으로 생성되면, 서버의 인메모리 rooms 배열에도 추가 (캐시 역할) // 인메모리 rooms 배열 동기화
                        pthread_mutex_lock(&rooms_mutex); // rooms 배열 접근을 위해 뮤텍스 잠금
                        int room_found = 0; // 빈 방 슬롯을 찾았는지 여부
                        for (int i = 0; i < MAX_ROOMS; i++) { // 모든 방 슬롯 순회
                            if (!rooms[i].active) { // 비활성(미사용) 슬롯이면
                                rooms[i].id = new_room_id; // 새 방 ID 할당
                                strncpy(rooms[i].name, msg.target, USERNAME_SIZE - 1); // 방 이름 복사
                                rooms[i].name[USERNAME_SIZE - 1] = '\0'; // 널 종료
                                rooms[i].active = 1; // 방 활성화
                                // ⭐ 수정: 인메모리 rooms 배열의 room_type도 1로 변경 // 그룹/개별 방으로 설정
                                rooms[i].room_type = 1; // 그룹/개별
                                rooms[i].creator_pk_id = clients[client_index].user_pk_id; // 생성자 pk_id 저장
                                room_count++; // 인메모리 방 개수 업데이트
                                room_found = 1; // 방 추가 완료 표시
                                break; // 반복문 종료
                            }
                        }
                        pthread_mutex_unlock(&rooms_mutex); // rooms 배열에 대한 뮤텍스 잠금 해제

                        char response_msg[200]; // 클라이언트에게 보낼 응답 메시지 버퍼 선언
                        snprintf(response_msg, sizeof(response_msg), "채팅방 '%s' (번호: %d)이 생성되었습니다!", 
                                 msg.target, new_room_id); // 생성된 방 이름과 번호를 포함한 메시지 생성
                        send_message_to_client(client_index, MSG_CREATE_ROOM, response_msg, "서버"); // 클라이언트에게 채팅방 생성 성공 메시지 전송

                        printf("[%s] 새 채팅방 생성 완료: '%s' (ID: %d) by %s (PK: %d)\n", 
                               __TIME__, msg.target, new_room_id, clients[client_index].nickname, clients[client_index].user_pk_id); // 서버 콘솔에 생성 완료 로그 출력
                    } else if (result == -2) { // 방 이름이 이미 존재하는 경우
                        send_message_to_client(client_index, MSG_ERROR, 
                            "동일한 이름의 채팅방이 이미 존재합니다.", "서버"); // 클라이언트에게 중복 에러 메시지 전송
                        printf("[%s] 채팅방 생성 실패: %s (이름 중복)\n", __TIME__, msg.target); // 서버 콘솔에 중복 실패 로그 출력
                    } else { // DB 오류 등 기타 실패
                        send_message_to_client(client_index, MSG_ERROR, 
                            "채팅방 생성에 실패했습니다. (DB 오류)", "서버"); // 클라이언트에게 DB 오류 메시지 전송
                        printf("[%s] 채팅방 생성 실패: %s (DB 오류)\n", __TIME__, msg.target); // 서버 콘솔에 DB 오류 로그 출력
                    }
                } else { // 로그인하지 않은 사용자가 방 생성 시도
                    send_message_to_client(client_index, MSG_ERROR, "로그인 후 채팅방을 만들 수 있습니다.", "서버"); // 로그인 필요 메시지 전송
                }
                break; // MSG_CREATE_ROOM 처리 종료

            case MSG_JOIN_ROOM: // 채팅방 입장 요청 메시지 처리 시작
                if (clients[client_index].logged_in) { // 로그인한 사용자만 입장 가능
                    printf("[%s] 채팅방 입장 요청: 방 ID %d (by %s)\n", __TIME__, msg.room_id, clients[client_index].nickname); // 서버 콘솔에 입장 요청 로그 출력

                    // 1. 먼저 DB에서 해당 room_id가 유효한지 확인하고 방 이름 가져오기
                    char query_room_name[BUFFER_SIZE]; // 방 이름을 조회할 SQL 쿼리문을 저장할 버퍼 선언
                    MYSQL_RES *res_room_name; // 쿼리 결과를 저장할 MySQL 결과 집합 포인터
                    MYSQL_ROW row_room_name; // 결과 집합에서 한 행(row)을 가리키는 포인터
                    char room_name_from_db[USERNAME_SIZE] = ""; // DB에서 읽어온 방 이름을 저장할 버퍼 (초기화)

                    snprintf(query_room_name, sizeof(query_room_name), "SELECT room_name FROM chatroom WHERE room_id = %d", msg.room_id); // room_id로 방 이름을 조회하는 쿼리문 생성
                    if (mysql_query(conn, query_room_name)) { // 쿼리 실행, 실패 시
                        fprintf(stderr, "[%s] 방 유효성 검사 쿼리 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 로그 출력
                        send_message_to_client(client_index, MSG_ERROR, "채팅방 유효성 검사에 실패했습니다.", "서버"); // 클라이언트에 에러 메시지 전송
                        break; // 현재 case문 종료
                    }
                    res_room_name = mysql_store_result(conn); // 쿼리 결과를 MySQL 결과 집합으로 저장
                    if (res_room_name == NULL || mysql_num_rows(res_room_name) == 0) { // 결과가 없거나 방이 존재하지 않으면
                        if (res_room_name) mysql_free_result(res_room_name); // 결과 집합이 있으면 해제
                        send_message_to_client(client_index, MSG_ERROR, "존재하지 않는 채팅방입니다.", "서버"); // 클라이언트에 에러 메시지 전송
                        printf("[%s] 채팅방 입장 실패: 방 %d (존재하지 않음)\n", __TIME__, msg.room_id); // 서버 콘솔에 실패 로그 출력
                        break; // 현재 case문 종료
                    }
                    row_room_name = mysql_fetch_row(res_room_name); // 결과 집합에서 첫 번째 행(row) 가져오기
                    strncpy(room_name_from_db, row_room_name[0], USERNAME_SIZE - 1); // 방 이름을 room_name_from_db에 복사 (최대 USERNAME_SIZE-1)
                    room_name_from_db[USERNAME_SIZE - 1] = '\0'; // 널 종료 문자 추가 (버퍼 오버플로우 방지)
                    mysql_free_result(res_room_name); // 결과 집합 메모리 해제

                    // 2. chatroom_user 테이블에 사용자-방 관계 삽입 (입장 기록)
                    int result = db_join_chatroom_user(msg.room_id, clients[client_index].user_pk_id); // chatroom_user 테이블에 사용자-방 관계를 추가하여 입장 기록(DB)

                    if (result == 0) { // 성공적으로 입장 기록이 추가된 경우
                        pthread_mutex_lock(&clients_mutex); // 클라이언트 배열 보호를 위해 뮤텍스 잠금
                        clients[client_index].room_id = msg.room_id; // 클라이언트의 현재 방 ID를 새로 입장한 방으로 업데이트
                        pthread_mutex_unlock(&clients_mutex); // 뮤텍스 해제
                        
                        char response_msg[200]; // 클라이언트에게 보낼 응답 메시지 버퍼 선언
                        snprintf(response_msg, sizeof(response_msg), "채팅방 '%s' (번호: %d)에 입장했습니다!", 
                                 room_name_from_db, msg.room_id); // DB에서 가져온 방 이름과 번호로 메시지 생성
                        send_message_to_client(client_index, MSG_JOIN_ROOM, response_msg, "서버"); // 클라이언트에게 입장 성공 메시지 전송
                        
                        printf("[%s] 방 입장 완료: %s (PK: %d) -> 방 %d ('%s')\n", 
                               __TIME__, clients[client_index].nickname, clients[client_index].user_pk_id, msg.room_id, room_name_from_db); // 서버 콘솔에 입장 완료 로그 출력
                    } else if (result == -2) { // 이미 해당 방에 입장해 있는 경우
                        send_message_to_client(client_index, MSG_ERROR, 
                            "이미 해당 채팅방에 입장해 있습니다.", "서버"); // 클라이언트에게 중복 입장 에러 메시지 전송
                        printf("[%s] 채팅방 입장 실패: 방 %d (이미 입장함) by %s\n", __TIME__, msg.room_id, clients[client_index].nickname); // 서버 콘솔에 중복 입장 실패 로그 출력
                    } else { // DB 오류 등 기타 실패
                        send_message_to_client(client_index, MSG_ERROR, 
                            "채팅방 입장에 실패했습니다. (DB 오류)", "서버"); // 클라이언트에게 DB 오류 메시지 전송
                        printf("[%s] 채팅방 입장 실패: 방 %d (DB 오류) by %s\n", __TIME__, msg.room_id, clients[client_index].nickname); // 서버 콘솔에 DB 오류 로그 출력
                    }
                } else { // 로그인하지 않은 사용자가 방 입장 시도
                    send_message_to_client(client_index, MSG_ERROR, "로그인 후 채팅방에 입장할 수 있습니다.", "서버"); // 로그인 필요 메시지 전송
                }
                break; // MSG_JOIN_ROOM 처리 종료
                
            case MSG_LEAVE_ROOM: // 채팅방 퇴장 요청 메시지 처리 시작
                if (clients[client_index].logged_in) { // 로그인한 사용자만 퇴장 가능
                    int old_room_id = clients[client_index].room_id; // 현재 클라이언트가 속한 방 ID 저장
                    if (old_room_id == 0) { // 이미 공개 채팅방에 있는 경우
                        send_message_to_client(client_index, MSG_ERROR, "이미 공개 채팅방에 있습니다!", "서버"); // 클라이언트에게 안내 메시지 전송
                        printf("[%s] 방 퇴장 요청: %s (이미 공개 채팅방)\n", __TIME__, clients[client_index].nickname); // 서버 콘솔에 안내 로그 출력
                        break; // 더 이상 처리하지 않고 종료
                    }
                    printf("[%s] 채팅방 퇴장 요청: 방 ID %d (by %s)\n", __TIME__, old_room_id, clients[client_index].nickname); // 서버 콘솔에 퇴장 요청 로그 출력

                    // chatroom_user 테이블에서 사용자-방 관계 삭제 (퇴장 기록)
                    int result = db_leave_chatroom_user(old_room_id, clients[client_index].user_pk_id); // DB에서 해당 사용자의 방 퇴장 처리

                    if (result == 0) { // 성공
                        pthread_mutex_lock(&clients_mutex); // 클라이언트 배열 보호를 위해 뮤텍스 잠금
                        clients[client_index].room_id = 0; // 클라이언트의 현재 방 ID를 공개 채팅방(0)으로 변경
                        pthread_mutex_unlock(&clients_mutex); // 뮤텍스 해제
                        
                        char response_msg[200]; // 클라이언트에게 보낼 응답 메시지 버퍼 선언
                        snprintf(response_msg, sizeof(response_msg), "채팅방 %d에서 나가 공개 채팅방으로 이동했습니다!", old_room_id); // 퇴장 안내 메시지 생성
                        send_message_to_client(client_index, MSG_LEAVE_ROOM, response_msg, "서버"); // 클라이언트에게 퇴장 성공 메시지 전송
                        
                        printf("[%s] 방 퇴장 완료: %s (PK: %d) <- 방 %d\n", 
                               __TIME__, clients[client_index].nickname, clients[client_index].user_pk_id, old_room_id); // 서버 콘솔에 퇴장 완료 로그 출력
                    } else { // DB 오류 등 실패 시
                        send_message_to_client(client_index, MSG_ERROR, 
                            "채팅방 퇴장에 실패했습니다. (DB 오류)", "서버"); // 클라이언트에게 DB 오류 메시지 전송
                        printf("[%s] 채팅방 퇴장 실패: 방 %d (DB 오류) by %s\n", __TIME__, old_room_id, clients[client_index].nickname); // 서버 콘솔에 실패 로그 출력
                    }
                } else { // 로그인하지 않은 사용자가 방 퇴장 시도 시
                    send_message_to_client(client_index, MSG_ERROR, "로그인 후 방을 나갈 수 있습니다.", "서버"); // 로그인 필요 메시지 전송
                }
                break;
                
            case MSG_ROOM_CHAT: // MSG_ROOM_CHAT 메시지 타입 처리 (채팅방 내 채팅)
                if (clients[client_index].logged_in && clients[client_index].room_id > 0) { // 로그인되어 있고, 방에 입장한 경우만 처리
                    printf("[%s] [방 %d] %s: %s\n",  // 서버 콘솔에 방 번호, 닉네임, 메시지 내용 출력
                           __TIME__, clients[client_index].room_id, 
                           clients[client_index].nickname, msg.content); // 닉네임으로 출력
                    // 채팅방 메시지 DB 저장
                    db_insert_chat_message(clients[client_index].room_id,  // 현재 클라이언트가 속한 방 ID
                                           clients[client_index].user_pk_id, msg.content); // 발신자 PK와 메시지 내용 저장
                    broadcast_message(MSG_ROOM_CHAT, msg.content,  // 같은 방에 있는 다른 클라이언트들에게 메시지 브로드캐스트
                        clients[client_index].nickname, client_index, clients[client_index].room_id); // 닉네임으로 전송, 본인 제외
                } else if (clients[client_index].logged_in && clients[client_index].room_id == 0) { // 로그인은 했지만 방에 입장하지 않은 경우
                    // 개인 채팅방에 있지 않은데 MSG_ROOM_CHAT을 보낸 경우 (클라이언트 버그 또는 잘못된 사용)
                    send_message_to_client(client_index, MSG_ERROR, "개인 채팅방에 입장해야만 방 메시지를 보낼 수 있습니다.", "서버"); // 에러 메시지 전송
                }
                break; // MSG_ROOM_CHAT 처리 종료
                
            case MSG_LIST_USERS: // MSG_LIST_USERS 메시지 타입 처리 (사용자 목록 요청)
                if (clients[client_index].logged_in) { // 로그인된 사용자만 목록 요청 가능
                    list_users(client_index); // 사용자 목록 전송 함수 호출
                }
                break; // MSG_LIST_USERS 처리 종료
                
            case MSG_LIST_ROOMS: // MSG_LIST_ROOMS 메시지 타입 처리 (채팅방 목록 요청)
                if (clients[client_index].logged_in) { // 로그인된 사용자만 목록 요청 가능
                    list_rooms(client_index); // 채팅방 목록 전송 함수 호출
                }
                break; // MSG_LIST_ROOMS 처리 종료
        }
    }
    
    // 클라이언트 정리
    if (clients[client_index].logged_in) { // 클라이언트가 로그인 상태인지 확인
        char leave_msg[200]; // 퇴장 메시지를 저장할 버퍼 선언
        snprintf(leave_msg, sizeof(leave_msg), 
            "%s님이 채팅방을 나갔습니다.", clients[client_index].username); // 퇴장 메시지 생성 (사용자명 포함)
        broadcast_message(MSG_USER_LEAVE, leave_msg, "서버", client_index, 0); // 모든 클라이언트에게 퇴장 메시지 브로드캐스트 (본인 제외, 공개방)
        
        printf("[%s] 사용자 퇴장: %s (IP: %s)\n", 
               __TIME__, clients[client_index].username, clients[client_index].ip_address); // 서버 콘솔에 퇴장 로그 출력
    }
    
    remove_client(client_index); // 클라이언트 정보 및 소켓 정리 함수 호출
    return NULL; // 스레드 종료
}

void send_message_to_client(int client_index, MessageType type, const char* content, const char* sender) { // 특정 클라이언트에게 메시지 전송 함수
    if (client_index < 0 || client_index >= MAX_CLIENTS || clients[client_index].socket == -1) { // 인덱스 유효성 및 소켓 활성 여부 확인
        return; // 유효하지 않으면 함수 종료
    }
    
    Message msg; // 메시지 구조체 선언
    memset(&msg, 0, sizeof(Message)); // 구조체 초기화
    msg.type = type; // 메시지 타입 설정
    strncpy(msg.content, content, BUFFER_SIZE - 1); // 메시지 내용 복사 (버퍼 크기 제한)
    strncpy(msg.username, sender, USERNAME_SIZE - 1); // 발신자 정보 복사 (버퍼 크기 제한)
    msg.timestamp = time(NULL); // 현재 시각을 타임스탬프로 저장
    
    if (send(clients[client_index].socket, &msg, sizeof(Message), 0) < 0) { // 클라이언트 소켓으로 메시지 전송 시도
        printf("! 메시지 전송 실패: 클라이언트 %d\n", client_index); // 전송 실패 시 에러 로그 출력
    }
}

void broadcast_message(MessageType type, const char* content, const char* sender, int exclude_client, int room_id) { // 여러 클라이언트에게 메시지 브로드캐스트 함수
    pthread_mutex_lock(&clients_mutex); // 클라이언트 배열 보호를 위해 뮤텍스 잠금
    for (int i = 0; i < MAX_CLIENTS; i++) { // 모든 클라이언트 순회
        if (i != exclude_client && clients[i].socket != -1 && clients[i].logged_in) { // 제외 대상이 아니고, 소켓 활성 및 로그인 상태인 경우
            if (room_id == 0 || clients[i].room_id == room_id) { // 공개방이거나, 해당 방에 있는 경우만
                send_message_to_client(i, type, content, sender); // 해당 클라이언트에게 메시지 전송
            }
        }
    }
    pthread_mutex_unlock(&clients_mutex); // 클라이언트 배열 보호용 뮤텍스 해제
}

// MySQL 데이터베이스 연결 함수
void db_connect() {
    conn = mysql_init(NULL); // MySQL 연결 객체 초기화 (conn은 전역 변수)
    if (conn == NULL) { // 초기화 실패 시
        fprintf(stderr, "[%s] mysql_init() 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        exit(EXIT_FAILURE); // 서버 비정상 종료
    }

    // 데이터베이스에 연결 시도
    if (mysql_real_connect(conn, DB_HOST, DB_USER, DB_PASS, DB_NAME, DB_PORT, DB_SOCKET, DB_CLIENT_FLAGS) == NULL) { // DB 연결
        fprintf(stderr, "[%s] MySQL 연결 실패: %s\n", __TIME__, mysql_error(conn)); // 연결 실패 시 에러 출력
        mysql_close(conn); // 연결 객체 해제
        exit(EXIT_FAILURE); // 서버 비정상 종료
    }
    printf("[%s] MySQL 데이터베이스에 성공적으로 연결되었습니다.\n", __TIME__); // 연결 성공 메시지

    // 캐릭터 셋을 UTF-8로 설정 (한글 등 멀티바이트 문자 깨짐 방지)
    if (mysql_set_character_set(conn, "utf8mb4")) { // 문자셋을 utf8mb4로 설정
        fprintf(stderr, "[%s] mysql_set_character_set 실패: %s\n", __TIME__, mysql_error(conn)); // 실패 시 에러 출력
    }
}

// 공개 채팅방의 room_id를 DB에서 로드하는 함수
int db_load_public_chat_room_id() {
    MYSQL_RES *res; // 쿼리 결과를 저장할 MySQL 결과 집합 포인터
    MYSQL_ROW row; // 결과 집합에서 한 행(row)을 가리키는 포인터
    char query[256]; // 쿼리문을 저장할 버퍼

    // room_type이 0인 방을 찾습니다. (공개 채팅방으로 약속)
    snprintf(query, sizeof(query), "SELECT room_id FROM chatroom WHERE room_type = 0 LIMIT 1"); // room_type이 0(공개방)인 채팅방의 room_id를 하나만 조회하는 쿼리문 생성

    if (mysql_query(conn, query)) { // 쿼리 실행, 실패 시
        fprintf(stderr, "[%s] 공개 채팅방 ID 로드 쿼리 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        return -1; // 실패 시 -1 반환
    }

    res = mysql_store_result(conn); // 쿼리 결과를 MySQL 결과 집합으로 저장
    if (res == NULL) { // 결과 집합이 NULL이면
        fprintf(stderr, "[%s] 공개 채팅방 ID 로드 결과 저장 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        return -1; // 실패 시 -1 반환
    }

    if ((row = mysql_fetch_row(res)) != NULL) { // 결과 집합에서 한 행(row)을 가져와서 NULL이 아니면(즉, 공개방이 존재하면)
        g_public_chat_room_id = atoi(row[0]); // row[0]에 있는 room_id 값을 정수로 변환하여 전역 변수에 저장
        printf("[%s] 공개 채팅방 ID 로드 성공: %d\n", __TIME__, g_public_chat_room_id); // 성공 로그 출력
    } else { // 결과가 없으면(공개방이 없으면)
        fprintf(stderr, "[%s] room_type이 0인 공개 채팅방을 찾을 수 없습니다. 수동으로 생성해야 합니다.\n", __TIME__); // 에러 메시지 출력
        g_public_chat_room_id = -1; // 찾지 못했음을 -1로 표시
    }

    mysql_free_result(res); // 결과 집합 메모리 해제
    return (g_public_chat_room_id != -1) ? 0 : -1; // 공개방 ID가 유효하면 0, 아니면 -1 반환
}

// 새 사용자 등록 (users 테이블에 INSERT)
// pk_id_out: 새로 삽입된 레코드의 AUTO_INCREMENT pk_id를 반환
// 반환값: 0 성공, -1 DB 오류, -2 이미 존재하는 사용자명
int db_register_user(const char* username, const char* password_hash, const char* nickname, int* pk_id_out) {
    char query[BUFFER_SIZE * 2]; // 쿼리 버퍼 (충분한 크기)
    MYSQL_RES *res;              // MySQL 쿼리 결과를 저장할 포인터
    MYSQL_ROW row;               // 결과 집합에서 한 행(row)을 가리키는 포인터

    // 1. 사용자명 (id) 중복 체크
    snprintf(query, sizeof(query), "SELECT pk_id FROM users WHERE id = '%s'", username); // 입력된 사용자명으로 이미 등록된 사용자가 있는지 조회하는 쿼리 생성
    if (mysql_query(conn, query)) { // 쿼리 실행, 실패 시
        fprintf(stderr, "[%s] 사용자명 중복 체크 쿼리 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        return -1; // DB 오류 발생 시 -1 반환
    }
    res = mysql_store_result(conn); // 쿼리 결과 저장
    if (res && mysql_num_rows(res) > 0) { // 결과가 있고, 행이 존재하면(이미 존재하는 사용자명)
        mysql_free_result(res); // 결과 집합 해제
        return -2; // 이미 존재하는 사용자명이므로 -2 반환
    }
    if (res) mysql_free_result(res); // 결과 집합 해제 (결과가 없을 수도 있으므로 확인)

    // 2. 새 사용자 삽입 (INSERT 쿼리)
    // password는 이미 해싱된 값이 들어옴. created_at은 현재 UNIX 타임스탬프
    snprintf(query, sizeof(query), 
             "INSERT INTO users (id, password, nickname, created_at) VALUES ('%s', '%s', '%s', %ld)", // 새 사용자 정보를 INSERT하는 쿼리 생성
             username, password_hash, nickname, (long)time(NULL)); // username, 해싱된 비밀번호, 닉네임, 생성 시각(UNIX 타임스탬프) 사용
    
    if (mysql_query(conn, query)) { // INSERT 쿼리 실행, 실패 시
        fprintf(stderr, "[%s] 사용자 등록 쿼리 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        return -1; // DB 오류 발생 시 -1 반환
    }

    // 3. 삽입된 레코드의 AUTO_INCREMENT pk_id 값 가져오기
    *pk_id_out = mysql_insert_id(conn); // 마지막으로 삽입된 AUTO_INCREMENT ID를 pk_id_out에 저장
    return 0; // 성공적으로 등록되었으므로 0 반환
}

// 사용자 인증 (users 테이블 조회)
// pk_id_out: 인증 성공 시 해당 사용자의 pk_id 반환
// nickname_out: 인증 성공 시 해당 사용자의 nickname 반환
// 반환값: 1 성공, 0 실패 (인증 정보 불일치 또는 DB 오류)
int db_authenticate_user(const char* username, const char* password_hash, int* pk_id_out, char* nickname_out) {
    char query[BUFFER_SIZE * 2];                                   // SQL 쿼리문을 저장할 버퍼 선언
    MYSQL_RES *res;                                                // 쿼리 결과를 저장할 MySQL 결과 집합 포인터
    MYSQL_ROW row;                                                 // 결과 집합에서 한 행(row)을 가리키는 포인터

    // 사용자명과 해싱된 비밀번호가 일치하는 레코드 조회
    snprintf(query, sizeof(query), "SELECT pk_id, nickname FROM users WHERE id = '%s' AND password = '%s'",
             username, password_hash);                             // 입력된 사용자명과 해싱된 비밀번호로 pk_id, nickname을 조회하는 쿼리 생성
    
    if (mysql_query(conn, query)) {                                // 쿼리 실행, 실패 시
        fprintf(stderr, "[%s] 사용자 인증 쿼리 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        return 0; // DB 오류 또는 인증 실패
    }

    res = mysql_store_result(conn);                                // 쿼리 결과를 MySQL 결과 집합으로 저장
    if (res == NULL) {                                             // 결과 집합이 NULL이면
        fprintf(stderr, "[%s] 결과 저장 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        return 0; // 결과 저장 실패 시 인증 실패 반환
    }

    if (mysql_num_rows(res) > 0) {                                 // 일치하는 레코드가 존재하면(즉, 인증 성공)
        row = mysql_fetch_row(res);                                // 첫 번째 행(row) 가져오기
        if (row[0] && row[1]) {                                    // pk_id와 nickname이 유효한지 확인
            *pk_id_out = atoi(row[0]);                             // pk_id를 int로 변환하여 반환
            strncpy(nickname_out, row[1], USERNAME_SIZE - 1);      // nickname을 nickname_out에 복사 (최대 USERNAME_SIZE-1)
            nickname_out[USERNAME_SIZE - 1] = '\0';                // 널 종료 문자 추가 (버퍼 오버플로우 방지)
            mysql_free_result(res);                                // 결과 집합 메모리 해제
            return 1; // 인증 성공
        }
    }
    
    mysql_free_result(res);                                        // 결과 집합 메모리 해제
    return 0; // 인증 실패 (일치하는 레코드 없음)
}

// 4서버.c 파일의 적절한 위치에 함수 구현
// 사용자 정보(ID 또는 비밀번호)를 업데이트하는 함수
// new_username이 NULL이면 사용자명 변경 안 함
// new_password_hash가 NULL이면 비밀번호 변경 안 함
// 반환: 0=성공, -1=DB 오류, -2=사용자명 중복
// 4서버.c 파일의 db_update_user_info 함수 내부
int db_update_user_info(int user_pk_id, const char* new_username, const char* new_password_hash) {
    char query[BUFFER_SIZE * 2];
    MYSQL_STMT *stmt;
    MYSQL_BIND bind[3];
    int bind_count = 0;
    
    // 디버깅: 전달된 값 확인 (이전 단계에서 추가했다면 그대로 유지)
    printf("[DEBUG] db_update_user_info called for user_pk_id: %d\n", user_pk_id);
    printf("[DEBUG] new_username (will map to 'id'): %s\n", (new_username ? new_username : "NULL (not changing)"));
    printf("[DEBUG] new_password_hash (will map to 'password'): %s\n", (new_password_hash ? "HASHED (not printed for security)" : "NULL (not changing)"));

    // 쿼리 문자열 동적 생성 - 컬럼 이름을 'id'와 'password'로 변경
    if (new_username != NULL && new_password_hash != NULL) {
        snprintf(query, sizeof(query),
                 "UPDATE users SET id = ?, password = ? WHERE pk_id = ?"); // <-- 여기 수정
    } else if (new_username != NULL) {
        snprintf(query, sizeof(query),
                 "UPDATE users SET id = ? WHERE pk_id = ?"); // <-- 여기 수정
    } else if (new_password_hash != NULL) {
        snprintf(query, sizeof(query),
                 "UPDATE users SET password = ? WHERE pk_id = ?"); // <-- 여기 수정
    } else {
        printf("[DEBUG] No changes requested for user_pk_id: %d\n", user_pk_id);
        return 0; // 변경할 내용이 없음
    }
    printf("[DEBUG] SQL Query: %s\n", query); // 생성된 SQL 쿼리 확인

    stmt = mysql_stmt_init(conn);
    if (!stmt) {
        fprintf(stderr, "[%s] mysql_stmt_init() failed: %s\n", __TIME__, mysql_error(conn));
        return -1;
    }

    if (mysql_stmt_prepare(stmt, query, strlen(query))) {
        fprintf(stderr, "[%s] mysql_stmt_prepare() failed: %s\n", __TIME__, mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    memset(bind, 0, sizeof(bind));

    // 바인딩 파라미터 설정 (이 부분은 변경할 필요 없음)
    // new_username은 이제 'id' 컬럼에, new_password_hash는 'password' 컬럼에 바인딩됩니다.
    if (new_username != NULL) {
        bind[bind_count].buffer_type = MYSQL_TYPE_STRING;
        bind[bind_count].buffer = (char*)new_username;
        bind[bind_count].buffer_length = strlen(new_username);
        bind[bind_count].is_null = 0;
        bind[bind_count].length = 0; 
        bind_count++;
    }
    if (new_password_hash != NULL) {
        bind[bind_count].buffer_type = MYSQL_TYPE_STRING;
        bind[bind_count].buffer = (char*)new_password_hash;
        bind[bind_count].buffer_length = strlen(new_password_hash);
        bind[bind_count].is_null = 0;
        bind[bind_count].length = 0; 
        bind_count++;
    }
    
    bind[bind_count].buffer_type = MYSQL_TYPE_LONG;
    bind[bind_count].buffer = (char*)&user_pk_id;
    bind[bind_count].is_null = 0;
    bind[bind_count].length = 0; 
    bind_count++;
    
    if (mysql_stmt_bind_param(stmt, bind)) {
        fprintf(stderr, "[%s] mysql_stmt_bind_param() failed: %s\n", __TIME__, mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return -1;
    }

    if (mysql_stmt_execute(stmt)) {
        fprintf(stderr, "[%s] mysql_stmt_execute() failed: %s\n", __TIME__, mysql_stmt_error(stmt));
        // 사용자명(이제 'id' 컬럼) 중복 오류 처리
        if (mysql_stmt_errno(stmt) == 1062) {
            mysql_stmt_close(stmt);
            return -2; // 사용자명(ID) 중복
        }
        mysql_stmt_close(stmt);
        return -1;
    }

    mysql_stmt_close(stmt);

    return 0; // 성공
}

// 새 채팅방 생성 (chatroom 테이블에 INSERT)
// room_id_out: 새로 삽입된 레코드의 AUTO_INCREMENT room_id를 반환
// 반환값: 0 성공, -1 DB 오류, -2 이미 존재하는 방 이름
int db_create_room(const char* room_name, int room_type, int creator_pk_id, int* room_id_out) {
    char query[BUFFER_SIZE * 2]; // SQL 쿼리문을 저장할 버퍼 선언
    MYSQL_RES *res; // 쿼리 결과를 저장할 MySQL 결과 집합 포인터

    // 1. 방 이름 중복 체크
    snprintf(query, sizeof(query), "SELECT room_id FROM chatroom WHERE room_name = '%s'", room_name); // 입력된 방 이름이 이미 존재하는지 확인하는 쿼리 생성
    if (mysql_query(conn, query)) { // 쿼리 실행, 실패 시
        fprintf(stderr, "[%s] 방 이름 중복 체크 쿼리 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        return -1; // DB 오류 발생 시 -1 반환
    }
    res = mysql_store_result(conn); // 쿼리 결과를 MySQL 결과 집합으로 저장
    if (res && mysql_num_rows(res) > 0) { // 결과가 있고, 이미 같은 이름의 방이 존재하면
        mysql_free_result(res); // 결과 집합 해제
        return -2; // 이미 존재하는 방 이름이므로 -2 반환
    }
    if (res) mysql_free_result(res); // 결과 집합 해제 (결과가 없을 수도 있으므로 확인)

    // 2. 새 채팅방 삽입
    snprintf(query, sizeof(query), 
             "INSERT INTO chatroom (room_name, room_type, creator_id, created_at) VALUES ('%s', %d, %d, %ld)", // 새 채팅방 정보를 INSERT하는 쿼리 생성
             room_name, room_type, creator_pk_id, (long)time(NULL)); // 방 이름, 타입, 생성자 PK, 생성 시각(UNIX 타임스탬프) 사용
    
    if (mysql_query(conn, query)) { // INSERT 쿼리 실행, 실패 시
        fprintf(stderr, "[%s] 채팅방 생성 쿼리 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        return -1; // DB 오류 발생 시 -1 반환
    }

    *room_id_out = mysql_insert_id(conn); // 새로 생성된 방의 AUTO_INCREMENT room_id 값을 room_id_out에 저장
    return 0; // 성공적으로 생성되었으므로 0 반환
}

// 채팅방 입장 (chatroom_user 테이블에 INSERT)
// 반환값: 0 성공, -1 DB 오류, -2 이미 입장해 있음
int db_join_chatroom_user(int room_id, int user_pk_id) {
    char query[BUFFER_SIZE];   // SQL 쿼리문을 저장할 버퍼 선언
    MYSQL_RES *res; // 쿼리 결과를 저장할 MySQL 결과 집합 포인터

    // 중복 입장 방지 체크
    snprintf(query, sizeof(query), "SELECT room_id FROM chatroom_user WHERE room_id = %d AND user_id = %d", room_id, user_pk_id); // 이미 해당 방에 입장했는지 확인하는 쿼리 생성
    if (mysql_query(conn, query)) { // 쿼리 실행, 실패 시
        fprintf(stderr, "[%s] chatroom_user 중복 체크 쿼리 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        return -1; // DB 오류 발생 시 -1 반환
    }
    res = mysql_store_result(conn); // 쿼리 결과를 MySQL 결과 집합으로 저장
    if (res && mysql_num_rows(res) > 0) { // 결과가 있고, 이미 입장해 있으면
        mysql_free_result(res); // 결과 집합 해제
        return -2; // 이미 입장해 있으므로 -2 반환
    }
    if (res) mysql_free_result(res); // 결과 집합 해제 (결과가 없을 수도 있으므로 확인)

    snprintf(query, sizeof(query), "INSERT INTO chatroom_user (room_id, user_id, joined_at) VALUES (%d, %d, %ld)", // chatroom_user 테이블에 입장 기록을 INSERT하는 쿼리 생성
             room_id, user_pk_id, (long)time(NULL)); // 방 ID, 사용자 PK, 입장 시각(UNIX 타임스탬프) 사용
    
    if (mysql_query(conn, query)) { // INSERT 쿼리 실행, 실패 시
        fprintf(stderr, "[%s] chatroom_user 삽입 쿼리 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        return -1; // DB 오류 발생 시 -1 반환
    }
    return 0; // 성공적으로 입장 기록이 추가되었으므로 0 반환
}

// 채팅방 퇴장 (chatroom_user 테이블에서 DELETE)
// 반환값: 0 성공, -1 DB 오류
int db_leave_chatroom_user(int room_id, int user_pk_id) { // chatroom_user 테이블에서 사용자의 방 퇴장(DELETE)
    char query[BUFFER_SIZE]; // SQL 쿼리문을 저장할 버퍼 선언
    snprintf(query, sizeof(query), "DELETE FROM chatroom_user WHERE room_id = %d AND user_id = %d", // room_id와 user_id가 일치하는 레코드 삭제 쿼리 생성
             room_id, user_pk_id); // 함수 인자로 받은 room_id, user_pk_id 사용
    
    if (mysql_query(conn, query)) { // 쿼리 실행, 실패 시
        fprintf(stderr, "[%s] chatroom_user 삭제 쿼리 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        return -1; // DB 오류 발생 시 -1 반환
    }
    return 0; // 성공적으로 삭제되었으므로 0 반환
}

// 채팅 메시지 저장 (chat_message 테이블에 INSERT)
// room_id: 0은 공개채팅, 그 외는 특정 채팅방 ID
// sender_pk_id: 발신자의 사용자 PK ID
// message_content: 메시지 내용
// 반환값: 0 성공, -1 DB 오류
int db_insert_chat_message(int room_id, int sender_pk_id, const char* message_content) { // chat_message 테이블에 메시지 저장 함수
    char query[BUFFER_SIZE * 2]; // SQL 쿼리문을 저장할 버퍼 선언 (충분히 크게)
    char escaped_content[BUFFER_SIZE * 2 + 1]; // SQL 인젝션 방지를 위한 이스케이프 문자열 버퍼

    // 메시지 내용 SQL 이스케이프
    mysql_real_escape_string(conn, escaped_content, message_content, strlen(message_content)); // message_content를 SQL 안전 문자열로 변환

    // SQL 쿼리 생성: send_at 컬럼에 (long)time(NULL)을 사용하여 Unix 타임스탬프를 삽입
    snprintf(query, sizeof(query), "INSERT INTO chat_message (room_id, sender_id, message, send_at) VALUES (%d, %d, '%s', %ld)", // INSERT 쿼리문 생성
             room_id, sender_pk_id, escaped_content, (long)time(NULL)); // 방 ID, 발신자 PK, 이스케이프된 메시지, 현재 시각(UNIX 타임스탬프) 사용
    
    if (mysql_query(conn, query)) { // 쿼리 실행, 실패 시
        fprintf(stderr, "[%s] 메시지 삽입 쿼리 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        return -1; // DB 오류 발생 시 -1 반환
    }
    return 0; // 성공적으로 저장되었으므로 0 반환
}

// MySQL 데이터베이스 연결 해제 함수
void db_disconnect() {
    if (conn != NULL) {
        mysql_close(conn); // MySQL 연결 객체 해제
        conn = NULL; // NULL로 설정하여 중복 해제 방지
        printf("[%s] MySQL 데이터베이스 연결이 해제되었습니다.\n", __TIME__);
    }
}

// 비밀번호를 SHA256으로 해싱하는 함수 (OpenSSL 라이브러리 필요)
// 컴파일 시 -lcrypto 옵션 필요 (gcc 4서버.c -o 4서버 -lmysqlclient -lcrypto -pthread)
void hash_password_sha256(const char* password, char* outputBuffer) {
    unsigned char hash[SHA256_DIGEST_LENGTH]; // SHA256 해시 값 (256비트 = 32바이트)
    SHA256_CTX sha256; // SHA256 컨텍스트 구조체

    SHA256_Init(&sha256); // SHA256 초기화
    SHA256_Update(&sha256, password, strlen(password)); // 비밀번호 데이터로 해시 업데이트
    SHA256_Final(hash, &sha256); // 해시 계산 완료

    // 계산된 해시 값을 16진수 문자열로 변환 (각 바이트 2자리 hex, 총 64자리)
    for(int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(outputBuffer + (i * 2), "%02x", hash[i]);
    }
    outputBuffer[SHA256_DIGEST_LENGTH * 2] = '\0'; // 문자열 끝에 NULL 문자 추가
}

// 사용자 등록 함수 (DB 연동)
// 기존 register_user 함수를 이 코드로 완전히 교체
int register_user(const char* username, const char* password) {
    char hashed_password[SHA256_DIGEST_LENGTH * 2 + 1];
    hash_password_sha256(password, hashed_password); // 비밀번호 해싱

    int pk_id; // 새로 생성된 사용자의 pk_id를 받을 변수
    // DB의 'id'와 'nickname'은 일단 username으로 동일하게 사용. 필요시 nickname도 클라이언트로부터 입력받도록 수정
    return db_register_user(username, hashed_password, username /* nickname */, &pk_id);
}

// 기존 authenticate_user 함수를 이 코드로 완전히 교체
int authenticate_user(const char* username, const char* password) {
    char hashed_password[SHA256_DIGEST_LENGTH * 2 + 1];
    hash_password_sha256(password, hashed_password); // 입력받은 비밀번호 해싱

    int pk_id; // 인증된 사용자의 pk_id를 받을 변수
    char nickname[USERNAME_SIZE]; // 인증된 사용자의 nickname을 받을 변수
    return db_authenticate_user(username, hashed_password, &pk_id, nickname);
}

// 클라이언트 닉네임으로 클라이언트 인덱스를 찾는 함수
// (Thread-safe 접근을 위해 clients_mutex를 사용합니다.)
int find_client_by_username(const char* nickname) {
    pthread_mutex_lock(&clients_mutex); // 클라이언트 배열 접근 시 뮤텍스 잠금
    for (int i = 0; i < MAX_CLIENTS; i++) {
        // 클라이언트 슬롯이 활성화(소켓이 -1이 아님)되어 있고,
        // 로그인 상태이며, 닉네임이 일치하는지 확인
        if (clients[i].socket != -1 && clients[i].logged_in && strcmp(clients[i].nickname, nickname) == 0) {
            pthread_mutex_unlock(&clients_mutex); // 찾았으면 뮤텍스 해제 후 인덱스 반환
            return i; 
        }
    }
    pthread_mutex_unlock(&clients_mutex); // 찾지 못했으면 뮤텍스 해제 후 -1 반환
    return -1; 
}

int find_client_by_socket(int socket) {                        // 소켓 번호로 클라이언트 인덱스를 찾는 함수
    for (int i = 0; i < MAX_CLIENTS; i++) {                    // 모든 클라이언트 슬롯을 순회
        if (clients[i].socket == socket) {                     // 해당 슬롯의 소켓이 입력값과 같으면
            return i;                                          // 인덱스 반환
        }
    }
    return -1;                                                 // 찾지 못하면 -1 반환
}

void remove_client(int client_index) {                         // 클라이언트 연결 해제 및 정보 초기화 함수
    pthread_mutex_lock(&clients_mutex);                        // 클라이언트 배열 보호를 위해 뮤텍스 잠금
    if (clients[client_index].socket != -1) {                  // 해당 슬롯이 활성화(연결됨) 상태라면
        printf("[DEBUG] 클라이언트 제거: 슬롯 #%d, IP=%s, 사용자=%s\n", 
               client_index, 
               clients[client_index].ip_address,
               clients[client_index].logged_in ? clients[client_index].username : "미로그인"); // 디버그 로그 출력
        
        close(clients[client_index].socket);                   // 소켓 연결 종료
        clients[client_index].socket = -1;                     // 소켓 번호를 -1로 초기화(비활성화)
        clients[client_index].logged_in = 0;                   // 로그인 상태 초기화
        clients[client_index].room_id = 0;                     // 방 정보 초기화
        memset(clients[client_index].username, 0, USERNAME_SIZE); // 사용자명 초기화
        memset(clients[client_index].ip_address, 0, INET_ADDRSTRLEN); // IP 주소 초기화
    }
    pthread_mutex_unlock(&clients_mutex);                      // 뮤텍스 해제
}

void create_room(int client_index, const char* room_name) {    // 새로운 채팅방을 생성하는 함수
    pthread_mutex_lock(&rooms_mutex);                          // 방 배열 보호를 위해 뮤텍스 잠금
    
    // 빈 방 슬롯 찾기
    int room_index = -1;                                       // 빈 방 슬롯 인덱스 변수 초기화
    for (int i = 0; i < MAX_ROOMS; i++) {                      // 모든 방 슬롯을 순회
        if (!rooms[i].active) {                                // 비활성(미사용) 슬롯이면
            room_index = i;                                    // 빈 슬롯 인덱스 저장
            rooms[i].active = 1;                               // 방을 활성화 상태로 변경
            strcpy(rooms[i].name, room_name);                  // 방 이름 저장
            room_count++;                                      // 방 개수 증가
            break;                                             // 반복문 종료
        }
    }
    
    pthread_mutex_unlock(&rooms_mutex);                        // 뮤텍스 해제
    
    if (room_index != -1) { // 빈 방 슬롯이 존재하면
        char msg[200]; // 메시지 버퍼 선언
        snprintf(msg, sizeof(msg), "채팅방 '%s' (번호: %d)이 생성되었습니다!",  // 생성된 방 정보를 메시지로 작성
                 room_name, rooms[room_index].id); // 방 이름과 ID를 메시지에 삽입
        send_message_to_client(client_index, MSG_CREATE_ROOM, msg, "서버"); // 클라이언트에게 방 생성 성공 메시지 전송
        
        printf("[%s] 새 채팅방 생성: '%s' (ID: %d) by %s\n",  // 서버 콘솔에 방 생성 로그 출력
               __TIME__, room_name, rooms[room_index].id, clients[client_index].username); // 시간, 방 이름, ID, 생성자 출력
    } else { // 빈 방 슬롯이 없으면
        send_message_to_client(client_index, MSG_ERROR,  // 클라이언트에게 에러 메시지 전송
            "더 이상 채팅방을 생성할 수 없습니다.", "서버"); // 방 생성 불가 안내 메시지
    }
}

void join_room(int client_index, int room_id) { // 클라이언트가 채팅방에 입장하는 함수
    pthread_mutex_lock(&rooms_mutex); // rooms 배열 보호를 위해 뮤텍스 잠금
    
    int room_index = room_id - 1; // room_id를 배열 인덱스로 변환 (1부터 시작한다고 가정)
    if (room_index >= 0 && room_index < MAX_ROOMS && rooms[room_index].active) { // 유효한 방이고 활성화된 경우
        pthread_mutex_lock(&clients_mutex); // 클라이언트 배열 보호를 위해 뮤텍스 잠금
        clients[client_index].room_id = room_id; // 해당 클라이언트의 room_id를 변경
        pthread_mutex_unlock(&clients_mutex); // 클라이언트 배열 뮤텍스 해제
        
        char msg[200]; // 메시지 버퍼 선언
        snprintf(msg, sizeof(msg), "채팅방 '%s' (번호: %d)에 입장했습니다!",  // 입장 성공 메시지 작성
                 rooms[room_index].name, room_id); // 방 이름과 ID를 메시지에 삽입
        send_message_to_client(client_index, MSG_JOIN_ROOM, msg, "서버"); // 클라이언트에게 입장 성공 메시지 전송
        
        printf("[%s] 방 입장: %s -> 방 %d ('%s')\n",  // 서버 콘솔에 입장 로그 출력
               __TIME__, clients[client_index].username, room_id, rooms[room_index].name); // 시간, 사용자, 방 ID, 방 이름 출력
    } else { // 방이 존재하지 않거나 비활성화된 경우
        send_message_to_client(client_index, MSG_ERROR,  // 클라이언트에게 에러 메시지 전송
            "존재하지 않는 채팅방입니다.", "서버"); // 방 없음 안내 메시지
    }
    
    pthread_mutex_unlock(&rooms_mutex); // rooms 배열 뮤텍스 해제
}

void leave_room(int client_index) { // 클라이언트가 채팅방을 나가는 함수
    pthread_mutex_lock(&clients_mutex); // 클라이언트 배열 보호를 위해 뮤텍스 잠금
    int old_room = clients[client_index].room_id; // 현재 클라이언트가 속한 방 ID 저장
    clients[client_index].room_id = 0; // 클라이언트의 room_id를 0(공개방)으로 변경
    pthread_mutex_unlock(&clients_mutex); // 클라이언트 배열 뮤텍스 해제
    
    if (old_room > 0) { // 기존에 방에 속해 있었다면
        char msg[200]; // 메시지 버퍼 선언
        snprintf(msg, sizeof(msg), "채팅방 %d에서 나가 공개 채팅방으로 이동했습니다!", old_room); // 퇴장 메시지 작성
        send_message_to_client(client_index, MSG_LEAVE_ROOM, msg, "서버"); // 클라이언트에게 퇴장 성공 메시지 전송
        
        printf("[%s] 방 퇴장: %s <- 방 %d\n",  // 서버 콘솔에 퇴장 로그 출력
               __TIME__, clients[client_index].username, old_room); // 시간, 사용자, 방 ID 출력
    }
}

void list_users(int client_index) { // 클라이언트에게 사용자 목록을 전송하는 함수
    if (!clients[client_index].logged_in) { // 클라이언트가 로그인하지 않은 경우
        send_message_to_client(client_index, MSG_ERROR, "로그인 후 접속자 목록을 볼 수 있습니다.", "서버"); // 에러 메시지 전송
        return; // 함수 종료
    }

    char user_list_content[BUFFER_SIZE * 4] = ""; // 더 큰 버퍼 사용 (모든 사용자 + 상태)
    char query[BUFFER_SIZE]; // SQL 쿼리문을 저장할 버퍼
    MYSQL_RES *res = NULL; // MySQL 쿼리 결과를 저장할 포인터
    MYSQL_ROW row; // 결과 집합에서 한 행(row)을 가리키는 포인터
    int total_users = 0; // 전체 사용자 수 카운트
    int online_count = 0; // 온라인 사용자 수 카운트

    // 1. DB에서 모든 등록된 사용자 정보 가져오기
    snprintf(query, sizeof(query), "SELECT id, nickname, pk_id FROM users ORDER BY id ASC"); // 사용자 id, 닉네임, pk_id를 조회하는 쿼리 생성
    if (mysql_query(conn, query)) { // 쿼리 실행, 실패 시
        fprintf(stderr, "[%s] 사용자 목록 조회 쿼리 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        send_message_to_client(client_index, MSG_ERROR, "사용자 목록을 불러오는 데 실패했습니다.", "서버"); // 에러 메시지 전송
        return; // 함수 종료
    }
    res = mysql_store_result(conn); // 쿼리 결과를 MySQL 결과 집합으로 저장
    if (res == NULL) { // 결과 집합이 NULL이면
        fprintf(stderr, "[%s] 사용자 목록 결과 저장 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        send_message_to_client(client_index, MSG_ERROR, "사용자 목록을 불러오는 데 실패했습니다.", "서버"); // 에러 메시지 전송
        return; // 함수 종료
    }

    // 헤더 추가
    snprintf(user_list_content, sizeof(user_list_content), "등록된 총 사용자: %lld명\n", mysql_num_rows(res)); // 전체 사용자 수를 헤더로 추가
    strcat(user_list_content, "───────────────────────────────────────────────\n"); // 구분선 추가

    pthread_mutex_lock(&clients_mutex); // 클라이언트 목록 접근 시 뮤텍스 락
    while ((row = mysql_fetch_row(res)) != NULL) { // 결과 집합에서 한 행씩 반복
        const char* user_id = row[0]; // 사용자 id
        const char* nickname = row[1]; // 닉네임
        int pk_id = atoi(row[2]); // pk_id를 정수로 변환
        char status[50]; // 상태 문자열 버퍼
        char temp[200]; // 한 줄 출력용 임시 버퍼
        int is_online = 0; // 온라인 여부 플래그
        char current_room_info[50] = ""; // 현재 방 정보 버퍼

        // 현재 접속 중인 클라이언트와 비교하여 온라인 상태 확인
        for (int i = 0; i < MAX_CLIENTS; i++) { // 모든 클라이언트 슬롯 순회
            if (clients[i].socket != -1 && clients[i].logged_in && clients[i].user_pk_id == pk_id) { // 해당 pk_id로 로그인한 클라이언트가 있으면
                is_online = 1; // 온라인 표시
                online_count++; // 온라인 카운트 증가
                if (clients[i].room_id == 0) { // 공개방에 있으면
                    strcpy(current_room_info, "공개"); // "공개"로 표시
                } else { // 특정 방에 있으면
                    snprintf(current_room_info, sizeof(current_room_info), "방 %d", clients[i].room_id); // 방 번호 표시
                }
                break; // 더 이상 검사하지 않고 종료
            }
        }

        if (is_online) { // 온라인이면
            snprintf(status, sizeof(status), "🟢 온라인 (현재 방: %s)", current_room_info); // 온라인 상태와 방 정보 표시
        } else { // 오프라인이면
            strcpy(status, "🔴 오프라인"); // 오프라인 상태 표시
        }

        snprintf(temp, sizeof(temp), "👤 ID: %-15s | 닉네임: %-15s | 상태: %s\n", 
                 user_id, nickname, status); // 한 줄 사용자 정보 생성
        
        // 버퍼 오버플로우 방지
        if (strlen(user_list_content) + strlen(temp) < sizeof(user_list_content) - 1) { // 버퍼가 충분하면
            strcat(user_list_content, temp); // 사용자 정보 추가
        } else { // 버퍼가 부족하면
            // 버퍼 부족 시 메시지 잘림 방지
            strcat(user_list_content, "...\n(목록이 너무 길어 일부만 표시됩니다.)\n"); // 잘림 안내 추가
            break; // 반복 종료
        }
        total_users++; // 전체 사용자 수 증가
    }
    pthread_mutex_unlock(&clients_mutex); // 뮤텍스 언락

    mysql_free_result(res); // 결과 집합 해제

    // 최종적으로 접속 중인 사용자 수 정보 추가
    char final_header[100]; // 접속자 수 헤더 버퍼
    snprintf(final_header, sizeof(final_header), "총 %d명 접속 중\n", online_count); // 온라인 사용자 수 표시
    
    // 기존 내용 앞에 추가하기 위해 임시 버퍼 사용
    char temp_full_list[BUFFER_SIZE * 4]; // 임시 전체 버퍼
    snprintf(temp_full_list, sizeof(temp_full_list), "%s%s", final_header, user_list_content); // 접속자 수 + 전체 목록 합치기
    strcpy(user_list_content, temp_full_list); // 최종 버퍼에 복사

    send_message_to_client(client_index, MSG_LIST_USERS, user_list_content, "서버"); // 클라이언트에게 사용자 목록 전송
    printf("[%s] 👥 접속자 목록 전송 완료 (요청: %s)\n", __TIME__, clients[client_index].nickname); // 서버 콘솔에 로그 출력
}

void list_rooms(int client_index) { // 클라이언트에게 채팅방 목록을 전송하는 함수
    if (!clients[client_index].logged_in) { // 클라이언트가 로그인하지 않은 경우
        send_message_to_client(client_index, MSG_ERROR, "로그인 후 채팅방 목록을 볼 수 있습니다.", "서버"); // 에러 메시지 전송
        return; // 함수 종료
    }

    char room_list_content[BUFFER_SIZE * 4] = ""; // 더 큰 버퍼 사용 (채팅방 목록 저장용)
    char query[BUFFER_SIZE * 2]; // 조인 쿼리를 위해 더 큰 버퍼 사용
    MYSQL_RES *res = NULL; // MySQL 쿼리 결과를 저장할 포인터
    MYSQL_ROW row; // 결과 집합에서 한 행(row)을 가리키는 포인터
    int active_rooms_count = 0; // 활성화된 채팅방 개수 카운트

    // DB에서 모든 채팅방 정보와 각 방의 사용자 수 가져오기
    snprintf(query, sizeof(query), 
             "SELECT c.room_id, c.room_name, COUNT(cu.user_id) AS user_count " // room_id, room_name, 사용자 수를 조회
             "FROM chatroom c LEFT JOIN chatroom_user cu ON c.room_id = cu.room_id " // chatroom과 chatroom_user 테이블을 LEFT JOIN
             "GROUP BY c.room_id, c.room_name ORDER BY c.room_id ASC"); // room_id, room_name별로 그룹화 및 정렬
    
    if (mysql_query(conn, query)) { // 쿼리 실행, 실패 시
        fprintf(stderr, "[%s] 채팅방 목록 조회 쿼리 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        send_message_to_client(client_index, MSG_ERROR, "채팅방 목록을 불러오는 데 실패했습니다.", "서버"); // 에러 메시지 전송
        return; // 함수 종료
    }
    res = mysql_store_result(conn); // 쿼리 결과를 MySQL 결과 집합으로 저장
    if (res == NULL) { // 결과 집합이 NULL이면
        fprintf(stderr, "[%s] 채팅방 목록 결과 저장 실패: %s\n", __TIME__, mysql_error(conn)); // 에러 메시지 출력
        send_message_to_client(client_index, MSG_ERROR, "채팅방 목록을 불러오는 데 실패했습니다.", "서버"); // 에러 메시지 전송
        return; // 함수 종료
    }

    // 헤더 추가
    snprintf(room_list_content, sizeof(room_list_content), "총 %lld개 채팅방 활성\n", mysql_num_rows(res)); // 전체 채팅방 수를 헤더로 추가
    strcat(room_list_content, "───────────────────────────────────────────────\n"); // 구분선 추가

    while ((row = mysql_fetch_row(res)) != NULL) { // 결과 집합에서 한 행씩 반복
        int room_id = atoi(row[0]); // room_id를 정수로 변환
        const char* room_name = row[1]; // 방 이름
        int users_in_room = atoi(row[2]); // 방에 접속한 사용자 수
        char temp[150]; // 한 줄 출력용 임시 버퍼

        snprintf(temp, sizeof(temp), "방 %-5d | 이름: %-20s | 접속자: %d명\n", 
                 room_id, room_name, users_in_room); // 한 줄 채팅방 정보 생성
        
        // 버퍼 오버플로우 방지
        if (strlen(room_list_content) + strlen(temp) < sizeof(room_list_content) - 1) { // 버퍼가 충분하면
            strcat(room_list_content, temp); // 채팅방 정보 추가
        } else { // 버퍼가 부족하면
            // 버퍼 부족 시 메시지 잘림 방지
            strcat(room_list_content, "...\n(목록이 너무 길어 일부만 표시됩니다.)\n"); // 잘림 안내 추가
            break; // 반복 종료
        }
        active_rooms_count++; // 활성화된 채팅방 개수 증가
    }
    mysql_free_result(res); // 결과 집합 해제

    if (active_rooms_count == 0) { // 활성화된 채팅방이 없으면
        strcpy(room_list_content, "현재 생성된 채팅방이 없습니다."); // 안내 메시지 설정
    }

    send_message_to_client(client_index, MSG_LIST_ROOMS, room_list_content, "서버"); // 클라이언트에게 채팅방 목록 전송
    printf("[%s] 채팅방 목록 전송 완료 (요청: %s)\n", __TIME__, clients[client_index].nickname); // 서버 콘솔에 로그 출력
}

void signal_handler(int sig) { // 시그널 핸들러 함수 (서버 종료 신호 처리)
    printf("\n[%s] 서버 종료 신호 수신... 정리 중...\n", __TIME__); // 종료 신호 수신 로그 출력
    cleanup_server(); // 서버 정리 함수 호출
    exit(0); // 프로세스 종료
}

void cleanup_server() { // 서버 종료 시 리소스 정리 함수
    printf("[%s] 서버 정리 중...\n", __TIME__); // 서버 정리 시작 로그 출력
    
    // 모든 클라이언트에게 종료 메시지 전송
    Message shutdown_msg; // 종료 메시지 구조체 선언
    memset(&shutdown_msg, 0, sizeof(Message)); // 구조체를 0으로 초기화
    shutdown_msg.type = MSG_ERROR; // 메시지 타입을 에러(종료 알림)로 설정
    strcpy(shutdown_msg.content, "서버가 종료됩니다. 연결이 끊어집니다."); // 종료 안내 메시지 설정
    
    pthread_mutex_lock(&clients_mutex); // 클라이언트 배열 보호를 위해 뮤텍스 잠금
    for (int i = 0; i < MAX_CLIENTS; i++) { // 모든 클라이언트 슬롯 순회
        if (clients[i].socket != -1) { // 소켓이 활성화된(연결된) 경우
            send(clients[i].socket, &shutdown_msg, sizeof(Message), 0); // 종료 메시지 전송
            close(clients[i].socket); // 클라이언트 소켓 닫기
            clients[i].socket = -1; // 소켓 번호를 -1로 초기화(비활성화)
        }
    }
    pthread_mutex_unlock(&clients_mutex); // 뮤텍스 해제
    
    // 서버 소켓 정리
    if (server_socket > 0) { // 서버 소켓이 유효한 경우
        close(server_socket); // 서버 소켓 닫기
    }

    // DB 연결 해제
    db_disconnect(); // DB 연결 해제 함수 호출
    
    // 뮤텍스 정리
    pthread_mutex_destroy(&clients_mutex); // 클라이언트 배열 뮤텍스 파괴
    pthread_mutex_destroy(&rooms_mutex); // 방 배열 뮤텍스 파괴
    
    printf("[%s] 채팅 서버가 종료되었습니다.\n", __TIME__); // 서버 종료 완료 로그 출력
}