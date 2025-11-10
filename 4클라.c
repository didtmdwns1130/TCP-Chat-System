#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <locale.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>

// 프로토콜 정의 (클라이언트와 서버가 완전히 동일해야 함)
#define DEFAULT_PORT 8080 // 클라이언트용. 서버에서는 직접 포트 번호 사용
#define MAX_CLIENTS 100   // 서버용
#define MAX_ROOMS 50      // 서버용
#define BUFFER_SIZE 1024
#define USERNAME_SIZE 50
#define PASSWORD_SIZE 100

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

// 전역 변수 (사용자님의 기존 변수명과 일치시켰습니다)
int server_socket;
char current_username[USERNAME_SIZE]; // 현재 클라이언트의 닉네임 (로그인 후 사용)
char server_ip[16];
int server_port;
int current_room = 0; // 현재 입장해 있는 채팅방 ID (0: 공개 채팅방)
int logged_in = 0;    // 로그인 상태 (0: 로그아웃, 1: 로그인)
int connection_active = 1; // 서버 연결 활성 상태 (0: 비활성, 1: 활성)
pthread_t receive_thread;
int last_action_was_register = 0; // 로그인/회원가입 후 메시지 처리를 위함
char logged_in_username[USERNAME_SIZE]; // 로그인 성공 시 서버로부터 받은 사용자 닉네임 (current_username과 동일 목적)
// ⭐⭐ 이 플래그를 전역 변수로 선언하여 상태를 유지하고 제어합니다. ⭐⭐
int menu_displayed_once_for_session = 0;

// 함수 선언
void* receive_messages(void* arg);
void send_message(MessageType type, const char* content, const char* target, int room_id);
void display_menu(); // 사용자님의 기존 print_menu 함수를 확장하여 모든 메뉴 처리
void handle_login_register_menu(); // 로그인/회원가입 메뉴를 전담하는 새 함수
void print_timestamp();
void clear_screen();
int connect_to_server();
void signal_handler(int sig);
void cleanup_client();
int safe_input(char* buffer, int size, const char* prompt);
void handle_menu_choice(int choice, const char* input_buffer_for_chat_message);
void handle_user_info_change();
void send_chat_message(const char* message_content);



int main(int argc, char* argv[]) {
    // 한글 지원 설정
    setlocale(LC_ALL, "");
    
    // 시그널 핸들러 설정
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN); // SIGPIPE 무시 (서버 연결 끊겼을 때 write 시 발생하는 오류 방지)
    
    // 서버 연결 정보 설정
    if (argc >= 3) {
        // 명령행 인자로 서버 IP와 포트 지정
        strncpy(server_ip, argv[1], sizeof(server_ip) - 1);
        server_ip[sizeof(server_ip) - 1] = '\0';
        server_port = atoi(argv[2]);
        
        if (server_port <= 0 || server_port > 65535) {
            printf("잘못된 포트 번호입니다. 1-65535 범위로 입력해주세요.\n");
            printf("사용법: %s <서버IP> <포트번호>\n", argv[0]);
            printf("예시: %s 192.168.1.100 8080\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    } else {
        // 사용자 입력으로 서버 정보 받기
        printf("═══════════════════════════════════════════════════════════════════\n");
        printf("                          서버 연결 설정\n");
        printf("═══════════════════════════════════════════════════════════════════\n");
        
        if (!safe_input(server_ip, sizeof(server_ip), "서버 IP 주소 (엔터 시 localhost): ")) {
            strcpy(server_ip, "127.0.0.1");
        }
        
        char port_str[10];
        if (!safe_input(port_str, sizeof(port_str), "서버 포트 번호 (엔터 시 8080): ")) {
            server_port = DEFAULT_PORT;
        } else {
            server_port = atoi(port_str);
            if (server_port <= 0 || server_port > 65535) {
                printf("잘못된 포트 번호입니다. 기본값 8080을 사용합니다.\n");
                server_port = DEFAULT_PORT;
            }
        }
    }

    // 서버 연결 시도
    printf("서버에 연결 중... (%s:%d)\n", server_ip, server_port);
    if (connect_to_server() != 0) {
        printf("서버 연결에 실패했습니다.\n");
        printf("확인사항:\n");
        printf("   - 서버가 실행 중인지 확인\n");
        printf("   - IP 주소와 포트 번호가 올바른지 확인\n");
        printf("   - 방화벽 설정 확인\n");
        exit(EXIT_FAILURE);
    }

    // 환영 메시지
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("                        간단한 채팅 프로그램  \n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("서버에 연결되었습니다: %s:%d\n", server_ip, server_port);
    printf("다른 사용자들과 실시간으로 채팅을 즐기세요!\n");
    printf("IP 중복 접속 방지 기능이 활성화되어 있습니다.\n");
    printf("═══════════════════════════════════════════════════════════════════\n\n");

    // 메시지 수신 스레드 시작
    if (pthread_create(&receive_thread, NULL, receive_messages, NULL) != 0) {
        printf("수신 스레드 생성에 실패했습니다.\n");
        cleanup_client();
        exit(EXIT_FAILURE);
    }

    while (connection_active) { 
        // 1. 로그인/회원가입 단계 루프 (로그인되어 있지 않고 연결 활성화 상태일 때)
        while (!logged_in && connection_active) {
            handle_login_register_menu(); 
            // handle_login_register_menu 내부에서 로그인 성공 시 logged_in = 1이 됨.
            // 또는 '3. 프로그램 종료' 선택 시 connection_active = 0이 됨.
            // 이 위치의 clear_screen()은 로그인/회원가입 메뉴 입력 후 바로 화면을 지우는 역할
            // 로그인 성공 시 메시지는 아래에서 출력하는 것이 더 자연스럽습니다.
        }

        // logged_in이 true 상태가 되면, 이 지점에 도달하게 됩니다.
        // 여기서 menu_displayed_once_for_session이 0으로 초기화되는 것이 맞습니다.
        // 로그아웃 시점에 0으로 명시적으로 변경하면 이 if 문은 불필요해질 수 있으나,
        // 안전을 위해 이중으로 0으로 세팅하는 것은 나쁘지 않습니다.
        // (단, !menu_displayed_once_for_session 조건이 걸려있으므로, if 문 안의 0 세팅은
        //  사실상 첫 로그인 시에만 작동합니다. 로그아웃 시점에 명시적 리셋이 더 중요합니다.)
        if (logged_in && !menu_displayed_once_for_session) { 
            clear_screen(); 
            printf("환영합니다! 로그인이 성공했습니다. 채팅을 시작합니다.\n"); 
            usleep(1000000); 
            clear_screen(); 
        }


        // 2. 메인 채팅 단계 루프 (로그인되어 있고 연결 활성화 상태일 때)
        while (logged_in && connection_active) {
            // static int menu_displayed_once = 0; // <--- 이 줄은 이미 삭제됨

            if (!menu_displayed_once_for_session) { // 새로운 플래그 사용
                display_menu(); // 처음 로그인했을 때 또는 'm' 입력 후 메뉴를 표시
                menu_displayed_once_for_session = 1;
            }
        
            char input_buffer[BUFFER_SIZE];
            int choice = -1;

            printf("메시지 입력 또는 메뉴 선택(m) > ");
            if (safe_input(input_buffer, BUFFER_SIZE, NULL) == 0) {
                usleep(100000); // 입력 실패 시 짧은 대기
                continue; // 다음 루프로 진행하여 다시 입력 대기
            }
        
            // 입력이 'm' (메뉴 다시 보기)인지 확인
            if (strcmp(input_buffer, "m") == 0 || strcmp(input_buffer, "M") == 0) {
                clear_screen(); // 화면 정리
                menu_displayed_once_for_session = 0; // 메뉴를 다시 표시하도록 플래그 리셋
                printf("메뉴를 다시 표시합니다.\n");
                usleep(500000); // 메시지 보여줄 시간
                continue; // 다음 루프로 진행
            }
        
            // 입력된 문자열이 숫자인지 확인 (메뉴 선택)
            int is_numeric = 1;
            for (int i = 0; i < strlen(input_buffer); i++) {
                if (!isdigit(input_buffer[i])) {
                    is_numeric = 0;
                    break;
                }
            }
        
            if (is_numeric) {
                choice = atoi(input_buffer);
                handle_menu_choice(choice, NULL); // 메뉴 선택 처리 함수 호출
                
                // 메뉴 선택 처리 후, 화면 정리
                clear_screen(); 

                // 로그인 상태를 유지하면서 로그아웃/종료가 아닌 다른 메뉴를 선택했을 경우
                // 다음 루프에서 메뉴가 다시 표시되도록 플래그를 0으로 설정
                // logged_in 변수는 handle_menu_choice에서 갱신될 수 있으므로 다시 확인
                if (logged_in && (choice != 7 && choice != 8)) { // 7: 로그아웃, 8: 프로그램 종료
                     menu_displayed_once_for_session = 0;
                }
                // 만약 choice가 7(로그아웃)이나 8(종료)이면, `logged_in`이 false가 되거나
                // `connection_active`가 false가 되어 이 내부 `while` 루프를 벗어나게 됩니다.
                // 그리고 외부 `while` 루프의 시작에서 `menu_displayed_once_for_session`이 다시 0으로
                // 초기화될 것이므로, 다음 로그인 시 메뉴가 정상적으로 표시됩니다.
            } else {
                // 숫자가 아니면 채팅 메시지로 간주
                send_chat_message(input_buffer); // 채팅 메시지 전송 함수 호출
                // 채팅 메시지 전송 후에는 메뉴를 다시 표시할 필요가 없으므로
                // menu_displayed_once_for_session 플래그를 그대로 1로 유지합니다.
                // 다음 루프에서 display_menu()는 호출되지 않고, 바로 입력 대기 상태로 넘어갑니다.
            }
        }       
    }
    cleanup_client();
    return 0;
}


int connect_to_server() {
    struct sockaddr_in server_addr;

    // 소켓 생성
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("소켓 생성 실패");
        return -1;
    }

    // 서버 주소 설정
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    
    // IP 주소 변환
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        printf("잘못된 IP 주소입니다: %s\n", server_ip);
        close(server_socket);
        return -1;
    }

    // 서버 연결 시도
    if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("서버 연결 실패: %s:%d\n", server_ip, server_port);
        close(server_socket);
        return -1;
    }

    return 0;
}

// 메시지 수신 스레드 함수
void *receive_messages(void *arg) {
    Message msg;
    char display_content[BUFFER_SIZE + USERNAME_SIZE + 50]; // 메시지 표시 버퍼

    // is_running 대신 connection_active 사용
    while (connection_active) {
        ssize_t bytes_received = recv(server_socket, &msg, sizeof(Message), 0);
        if (bytes_received <= 0) {
            if (bytes_received == 0) {
                print_timestamp();
                printf("서버와의 연결이 끊어졌습니다.\n");
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(10000); // 10ms 대기 후 재시도
                    continue;
                }
                // recv가 반환하는 오류 중 'Bad file descriptor' (EBADF)는 소켓이 닫혔을 때 발생합니다.
                // 이는 의도된 종료 상황일 수 있으므로 특별히 처리합니다.
                if (errno != EBADF) { // EBADF가 아니면 일반적인 오류로 간주
                    perror("메시지 수신 오류");
                }
            }
            connection_active = 0; // 연결 비활성화
            logged_in = 0;         // 로그인 상태 해제
            break;
        }

        // 로그인 메시지 등은 특별히 처리
        if (msg.type == MSG_LOGIN_SUCCESS) {
            logged_in = 1; 
            strncpy(logged_in_username, msg.username, USERNAME_SIZE - 1);
            logged_in_username[USERNAME_SIZE - 1] = '\0';
            strncpy(current_username, msg.username, USERNAME_SIZE - 1);
            current_username[USERNAME_SIZE - 1] = '\0';
            print_timestamp();
            printf("환영합니다! 로그인이 성공했습니다.\n");
            current_room = 0; 
            printf("⏎ 엔터를 눌러서 계속하세요...\n");
            getchar(); // 엔터 입력 대기
            continue; 
        } else if (msg.type == MSG_LOGIN_FAIL) {
            print_timestamp();
            printf("로그인 실패: %s\n", msg.content);
            logged_in = 0; 
            printf("⏎ 엔터를 눌러서 계속하세요...\n");
            getchar(); // 엔터 입력 대기
            continue;
        } else if (msg.type == MSG_REGISTER_SUCCESS) {
            print_timestamp();
            printf("회원가입이 완료되었습니다! 이제 로그인할 수 있습니다.\n");
            printf("⏎ 엔터를 눌러서 계속하세요...\n");
            getchar(); // 엔터 입력 대기
            continue;
        } else if (msg.type == MSG_ERROR) {
            print_timestamp();
            printf("오류: %s\n", msg.content);
            printf("⏎ 엔터를 눌러서 계속하세요...\n");
            getchar(); // 엔터 입력 대기
            continue;
        }

        // 로그인 상태일 때만 일반 메시지 처리
        if (logged_in) {
            // 모든 채팅 및 정보 메시지 출력 전에 print_timestamp() 호출
            // clear_screen()은 이곳에서 호출하지 않음으로써 메시지가 누적되게 합니다.
            switch (msg.type) {
                case MSG_PUBLIC_CHAT:
                    print_timestamp();
                    snprintf(display_content, sizeof(display_content), "[공개] %s: %s", msg.username, msg.content);
                    printf("%s\n", display_content);
                    break;
                
                case MSG_CREATE_ROOM: 
                    print_timestamp();
                    printf("%s\n", msg.content); 
                    break;

                case MSG_JOIN_ROOM: 
                    print_timestamp();
                    printf("%s\n", msg.content); 
                    {
                        char *id_start = strstr(msg.content, "(번호: ");
                        if (id_start) {
                            id_start += strlen("(번호: ");
                            current_room = atoi(id_start); 
                            if (current_room <= 0) { 
                                current_room = 0; 
                            }
                            print_timestamp();
                            printf("⭐ 현재 방 ID가 %d로 업데이트 되었습니다. ⭐\n", current_room);
                        } else {
                            current_room = 0; 
                            print_timestamp();
                            printf("채팅방 입장 성공 메시지에서 방 ID를 파싱할 수 없습니다. 공개방으로 이동.\n");
                        }
                    }
                    break;
                
                case MSG_LEAVE_ROOM: 
                    print_timestamp();
                    printf("%s\n", msg.content); 
                    current_room = 0; 
                    print_timestamp();
                    printf("현재 방 ID가 %d로 업데이트 되었습니다.\n", current_room);
                    break;

                case MSG_ROOM_CHAT:
                    printf("\n");
                    print_timestamp();
                    if (msg.room_id > 0) {
                        snprintf(display_content, sizeof(display_content), "[방 %d] %s: %s", msg.room_id, msg.username, msg.content);
                    } else {
                        snprintf(display_content, sizeof(display_content), "[방 채팅] %s: %s", msg.username, msg.content); 
                    }
                    printf("%s\n", display_content);
                    break;

                case MSG_PRIVATE_CHAT: 
                    print_timestamp();
                    snprintf(display_content, sizeof(display_content), "[귓속말] %s님으로부터: %s", msg.username, msg.content);
                    printf("%s\n", display_content);
                    break;

                case MSG_LIST_USERS:
                    print_timestamp();
                    printf("--- 사용자 목록 ---\n%s\n------------------\n", msg.content);
                    break;

                case MSG_LIST_ROOMS:
                    print_timestamp();
                    printf("--- 채팅방 목록 ---\n%s\n------------------\n", msg.content);
                    break;
                
                case MSG_DELETE_ROOM: 
                    print_timestamp();
                    printf("%s\n", msg.content); 
                    break;

                case MSG_USER_JOIN:
                    printf("\n");
                    print_timestamp();
                    break;

                case MSG_USER_CHANGE_SUCCESS: { // 사용자 정보 변경 성공
                print_timestamp();
                printf("내 정보 변경 성공: %s\n", msg.content); // msg.content에 서버가 보낸 성공 메시지
                // ID 변경 성공 시 클라이언트의 logged_in_username도 업데이트
                // 서버가 response_msg.target에 변경된 새 사용자명을 담아 보낼 것으로 가정
                if (strlen(msg.target) > 0) {
                    strncpy(logged_in_username, msg.target, USERNAME_SIZE - 1);
                    logged_in_username[USERNAME_SIZE - 1] = '\0';
                    printf("-> 새로운 사용자명: %s\n", logged_in_username);
                }
                break;
            }               
                case MSG_USER_LEAVE:
                    printf("\n");
                    print_timestamp();
                    break;

                default:
                    printf("\n");
                    print_timestamp();
                    printf("[알 수 없는 메시지] 타입: %d, 보낸이: %s, 내용: %s\n",
                           msg.type, msg.username, msg.content);
                    break;
            }
        }
    }
    return NULL;
}

// 메시지 전송 함수
void send_message(MessageType type, const char* content, const char* target, int room_id) {
    if (!connection_active) {
        printf("서버와의 연결이 끊어졌습니다.\n");
        return;
    }
    
    Message msg;
    memset(&msg, 0, sizeof(Message));
    msg.type = type;
    
    // 로그인된 사용자 이름은 모든 메시지에 포함 (서버에서 발신자를 식별하기 위함)
    strncpy(msg.username, logged_in_username, USERNAME_SIZE - 1); 
    msg.username[USERNAME_SIZE - 1] = '\0';

    if (content) {
        strncpy(msg.content, content, BUFFER_SIZE - 1);
        msg.content[BUFFER_SIZE - 1] = '\0';
    }
    if (target) {
        strncpy(msg.target, target, USERNAME_SIZE - 1);
        msg.target[USERNAME_SIZE - 1] = '\0';
    }
    
    msg.room_id = room_id;
    msg.timestamp = time(NULL);
    
    if (send(server_socket, &msg, sizeof(Message), 0) < 0) {
        printf("메시지 전송 실패\n");
        if (errno == EPIPE || errno == ECONNRESET) {
            connection_active = 0; // 연결 끊김 상태 업데이트
        }
    }
}

// send_chat_message 함수는 그대로 유지하거나 더 간략화할 수 있습니다.
void send_chat_message(const char* message_content) {
    if (strlen(message_content) == 0) {
        printf("빈 메시지는 보낼 수 없습니다.\n");
        return;
    }

    if (current_room == 0) {
        send_message(MSG_PUBLIC_CHAT, message_content, "", 0);
    } else {
        send_message(MSG_ROOM_CHAT, message_content, "", current_room);
    }

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    // logged_in_username은 로그인 시 저장된 전역 변수여야 합니다.
    printf("[%02d:%02d:%02d] [본인] %s: %s\n",
           t->tm_hour, t->tm_min, t->tm_sec,
           logged_in_username, // 내 아이디
           message_content); // 보낸 메시지 내용
    fflush(stdout); // 즉시 화면에 반영
}

// 이 함수는 print_menu 함수의 switch 문 내용을 가져옵니다.
void handle_menu_choice(int choice, const char* input_buffer_for_chat_message) {
    char chat_message[BUFFER_SIZE];
    char target_name[USERNAME_SIZE];
    int target_id;

    switch (choice) {
        case 1: // 채팅방 생성
            if (safe_input(target_name, USERNAME_SIZE, "생성할 채팅방 이름 입력: ") == 0) break;
            send_message(MSG_CREATE_ROOM, "", target_name, 0);
            printf("채팅방 생성 요청을 보냈습니다. 서버 응답을 기다립니다...\n");
            usleep(500000); // 사용자에게 메시지 보여줄 시간
            break;
        case 2: // 채팅방 나가기
            if (current_room > 0) {
                send_message(MSG_LEAVE_ROOM, "", "", current_room);
                printf("채팅방 나가기 요청을 보냈습니다. 서버 응답을 기다립니다...\n");
            } else {
                printf("이미 공개 채팅방에 있습니다!\n");
            }
            usleep(500000);
            break;
        case 3: // 사용자 목록 보기
            send_message(MSG_LIST_USERS, "", "", 0);
            printf("사용자 목록 요청을 보냈습니다. 목록을 받아오는 중...\n");
            getchar(); // 사용자가 엔터를 누를 때까지 대기
            break;
        case 4: // 채팅방 목록 보기
            send_message(MSG_LIST_ROOMS, "", "", 0);
            printf("채팅방 목록 요청을 보냈습니다. 목록을 받아오는 중...\n");
            getchar(); // 사용자가 엔터를 누를 때까지 대기
            break;
        case 5: // 채팅방 입장
            printf("입장할 채팅방 번호(ID) 입력: ");
            if (safe_input(chat_message, BUFFER_SIZE, NULL) == 0) break;
            target_id = atoi(chat_message);
            if (target_id <= 0) {
                printf("유효하지 않은 방 번호입니다. (0보다 큰 값을 입력하세요)\n");
                break;
            }
            if (target_id == current_room) {
                printf("이미 해당 채팅방에 입장해 있습니다!\n");
                break;
            }
            send_message(MSG_JOIN_ROOM, "", "", target_id);
            printf("채팅방 입장 요청을 보냈습니다. 서버 응답을 기다립니다...\n");
            getchar(); // 사용자가 엔터를 누를 때까지 대기
            break;
        case 6: // 귓속말 보내기
            if (safe_input(target_name, USERNAME_SIZE, "귓속말 대상 사용자 닉네임 입력: ") == 0) {
                printf("대상 닉네임을 입력해야 합니다.\n");
                break;
            }
            if (safe_input(chat_message, BUFFER_SIZE, "귓속말 내용 입력: ") == 0) {
                printf("귓속말 내용을 입력해야 합니다.\n");
                break;
            }
            send_message(MSG_PRIVATE_CHAT, chat_message, target_name, 0);
            printf("귓속말을 전송했습니다. 서버 응답을 기다립니다...\n");
            usleep(500000);
            break;
        case 7: // 사용자 정보 변경 (ID/비밀번호)
            handle_user_info_change();
            break;
        case 8: // 로그아웃
            send_message(MSG_LOGOUT, "", "", 0);
            logged_in = 0;
            // ⭐⭐ 중요: 로그아웃 시 메뉴 표시 플래그를 명시적으로 초기화 ⭐⭐
            menu_displayed_once_for_session = 0; 
            printf("로그아웃 되었습니다.\n");
            usleep(500000);
            break;
        case 9: // 종료
            printf("클라이언트를 종료합니다.\n");
            if (logged_in) {
                send_message(MSG_LOGOUT, "", "", 0);
            }
            connection_active = 0;
            logged_in = 0;
            // ⭐⭐ 종료 시에도 플래그 초기화 (안전용, 필수 아님) ⭐⭐
            menu_displayed_once_for_session = 0; 
            if (server_socket != -1) {
                shutdown(server_socket, SHUT_RDWR);
                close(server_socket);
                server_socket = -1;
            }
            break;
        default:
            printf("잘못된 선택입니다! 다시 시도해주세요.\n");
            usleep(500000);
            break;
    }
}

// 이 함수는 단순히 메뉴를 화면에 '그려주는' 역할만 합니다.
void display_menu() {
    clear_screen();
    printf("\n");
    printf("--- 현재 상태 ---\n");
    printf("[%02d:%02d:%02d] 환영합니다! (현재 방: %s)\n",
           localtime(&(time_t){time(NULL)})->tm_hour,
           localtime(&(time_t){time(NULL)})->tm_min,
           localtime(&(time_t){time(NULL)})->tm_sec,
           (current_room == 0 ? "공개 채팅방" : "개별/그룹 채팅방 ID"));
    if (current_room > 0) {
        printf("현재 채팅방 ID: %d\n", current_room);
    }
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("                             메인 메뉴\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf(" [1] 채팅방 생성\n");
    printf(" [2] 채팅방 나가기\n");
    printf(" [3] 사용자 목록 보기\n");
    printf(" [4] 채팅방 목록 보기\n");
    printf(" [5] 채팅방 입장\n");
    printf(" [6] 귓속말 보내기\n");
    printf(" [7] 사용자 정보 변경 (ID/비밀번호)\n");
    printf(" [8] 로그아웃\n");
    printf(" [9] 프로그램 종료\n");
    printf(" [m] 메뉴 다시 보기\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("Enter\n");
    fflush(stdout);
}


// 로그인/회원가입 메뉴를 처리하는 별도의 함수
void handle_login_register_menu() {
    char username[USERNAME_SIZE], password[PASSWORD_SIZE];
    int choice;

    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                            메인 메뉴                             ║\n");
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║  1.  로그인                                                      ║\n");
    printf("║  2.  회원가입                                                    ║\n");
    printf("║  3.  프로그램 종료                                               ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("선택: ");
    
    char choice_str[10];
    if (!fgets(choice_str, sizeof(choice_str), stdin)) {
        printf("입력 오류가 발생했습니다!\n");
        return;
    }
    
    choice = atoi(choice_str);
    
    switch (choice) {
        case 1: // 로그인
            printf("\n╔══════════════════════════════════════════════════════════════════╗\n");
            printf("║                              로그인                              ║\n");
            printf("╚══════════════════════════════════════════════════════════════════╝\n");
            if (!safe_input(username, sizeof(username), "ID: ")) {
                printf("ID을 입력해야 합니다!\n");
                return;
            }
            if (!safe_input(password, sizeof(password), "비밀번호: ")) {
                printf("비밀번호를 입력해야 합니다!\n");
                return;
            }
            Message login_msg;
            memset(&login_msg, 0, sizeof(Message));
            login_msg.type = MSG_LOGIN;
            strncpy(login_msg.username, username, USERNAME_SIZE - 1);
            login_msg.username[USERNAME_SIZE - 1] = '\0';
            strncpy(login_msg.password, password, PASSWORD_SIZE - 1);
            login_msg.password[PASSWORD_SIZE - 1] = '\0';
            
            last_action_was_register = 0; // 로그인 요청
            if (send(server_socket, &login_msg, sizeof(Message), 0) < 0) {
                printf("로그인 요청 전송에 실패했습니다!\n");
                return;
            }
            printf("⏳ 로그인 중입니다...\n");
            // 서버 응답은 receive_messages 스레드에서 처리하여 logged_in 변수를 업데이트합니다.
            break;
        case 2: // 회원가입
            printf("\n╔══════════════════════════════════════════════════════════════════╗\n");
            printf("║                            회원가입                              ║\n");
            printf("╚══════════════════════════════════════════════════════════════════╝\n");
            if (!safe_input(username, sizeof(username), "사용할 ID (3글자 이상): ")) {
                printf("ID를 입력해야 합니다!\n");
                return;
            }
            if (strlen(username) < 3) {
                printf("ID는 3글자 이상이어야 합니다!\n");
                return;
            }
            if (!safe_input(password, sizeof(password), "사용할 비밀번호 (3글자 이상): ")) {
                printf("비밀번호를 입력해야 합니다!\n");
                return;
            }
            if (strlen(password) < 3) {
                printf("비밀번호는 3글자 이상이어야 합니다!\n");
                return;
            }
            Message register_msg;
            memset(&register_msg, 0, sizeof(Message));
            register_msg.type = MSG_REGISTER;
            strncpy(register_msg.username, username, USERNAME_SIZE - 1);
            register_msg.username[USERNAME_SIZE - 1] = '\0';
            strncpy(register_msg.password, password, PASSWORD_SIZE - 1);
            register_msg.password[PASSWORD_SIZE - 1] = '\0';
            
            last_action_was_register = 1; // 회원가입 요청
            if (send(server_socket, &register_msg, sizeof(Message), 0) < 0) {
                printf("회원가입 요청 전송에 실패했습니다!\n");
                return;
            }
            printf("계정을 생성하고 있습니다...\n");
            // 서버 응답은 receive_messages 스레드에서 처리
            break;
        case 3: // 프로그램 종료
            connection_active = 0; // 연결 비활성화
            logged_in = 0;         // 로그인 상태 해제
            cleanup_client();
            exit(0);
        default:
            printf("잘못된 선택입니다! 1-3 중에서 선택해주세요.\n");
            break;
    }
    
    // 로그인/회원가입 요청 후 서버 응답을 기다리기 위해 잠시 대기
    getchar(); // Enter를 누를 때까지 대기
}

// 사용자 정보 변경 처리 함수
void handle_user_info_change() {
    char current_password_input[PASSWORD_SIZE]; // ★★★ 현재 비밀번호 입력을 위한 버퍼 추가 ★★★
    char new_id_input[USERNAME_SIZE];
    char new_password[PASSWORD_SIZE];
    char new_nickname[USERNAME_SIZE];
    Message msg_to_send;
    memset(&msg_to_send, 0, sizeof(Message));

    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("                         내 정보 변경\n");
    printf("═══════════════════════════════════════════════════════════════════\n");
    printf("-------------------------------------------------------------------\n");

    //현재 비밀번호를 입력받음
    if (!safe_input(current_password_input, sizeof(current_password_input), "현재 비밀번호를 입력하세요 : ")) {
        printf("현재 비밀번호를 입력해야 정보 변경을 진행할 수 없습니다.\n");
        usleep(1000000); // 1초 대기
        return;
    }

    // 새로운 ID, 비밀번호, 닉네임 입력 프롬프트
    if (!safe_input(new_id_input, sizeof(new_id_input), "새로운 ID (변경하지 않으려면 엔터) : ")) {
        strcpy(new_id_input, ""); // 변경하지 않으면 빈 문자열로 설정
    }

    if (!safe_input(new_password, sizeof(new_password), "새로운 비밀번호 (변경하지 않으려면 엔터) : ")) {
        strcpy(new_password, ""); // 변경하지 않으면 빈 문자열로 설정
    }


    // 변경할 내용이 있는지 확인 (현재 비밀번호 입력은 필수이므로 제외)
    if (strlen(new_id_input) == 0 && strlen(new_password) == 0 && strlen(new_nickname) == 0) {
        printf("변경할 내용이 없습니다. 현재 비밀번호만 입력되었습니다.\n");
        usleep(1000000); // 1초 대기
        return;
    }

    msg_to_send.type = MSG_USER_CHANGE;
    // 현재 로그인된 ID (서버에서 이 ID로 사용자를 찾음)
    strncpy(msg_to_send.username, logged_in_username, sizeof(msg_to_send.username) - 1); 
    msg_to_send.username[sizeof(msg_to_send.username) - 1] = '\0';

    // 현재 비밀번호를 msg_to_send.password 필드에 복사 (서버에서 검증)
    strncpy(msg_to_send.password, current_password_input, sizeof(msg_to_send.password) - 1);
    msg_to_send.password[sizeof(msg_to_send.password) - 1] = '\0';

    // 입력받은 새로운 ID를 메시지의 new_username 필드에 복사
    if (strlen(new_id_input) > 0) {
        strncpy(msg_to_send.new_username, new_id_input, sizeof(msg_to_send.new_username) - 1);
        msg_to_send.new_username[sizeof(msg_to_send.new_username) - 1] = '\0';
    } else {
        strcpy(msg_to_send.new_username, "");
    }

    // 입력받은 새로운 비밀번호를 메시지의 new_password 필드에 복사
    if (strlen(new_password) > 0) {
        strncpy(msg_to_send.new_password, new_password, sizeof(msg_to_send.new_password) - 1);
        msg_to_send.new_password[sizeof(msg_to_send.new_password) - 1] = '\0';
    } else {
        strcpy(msg_to_send.new_password, "");
    }

    if (send(server_socket, &msg_to_send, sizeof(Message), 0) == -1) {
        perror("send failed (MSG_USER_CHANGE)");
    } else {
        printf("정보 변경 요청을 서버에 전송했습니다...\n");
    }
    usleep(1000000); // 서버 응답을 기다릴 시간을 줌
}

void print_timestamp() {
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    printf("[%02d:%02d:%02d] ", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
}

void clear_screen() {
    #ifdef _WIN32
        system("cls");
    #else
        system("clear");
    #endif
}

int safe_input(char* buffer, int size, const char* prompt) {
    if (prompt) {
        printf("%s", prompt);
        fflush(stdout); // prompt를 즉시 출력
    }
    
    if (fgets(buffer, size, stdin) == NULL) {
        return 0;
    }
    
    // 개행 문자 제거
    buffer[strcspn(buffer, "\n")] = '\0';
    
    // 빈 문자열 체크
    return strlen(buffer) > 0;
}

void signal_handler(int sig) {
    printf("\n[%s] 클라이언트 종료 신호 수신\n", logged_in_username); // logged_in_username 사용
    cleanup_client();
    exit(0);
}

void cleanup_client() {
    connection_active = 0; // 연결 비활성 플래그 설정
    
    // 수신 스레드 정리
    // 스레드 취소는 안전하지 않을 수 있으므로, connection_active 플래그로 종료 유도
    if (receive_thread) {
        // pthread_cancel(receive_thread); // 스레드 취소는 사용하지 않는 것이 좋습니다.
        pthread_join(receive_thread, NULL); // 스레드 종료 대기
    }
    
    // 소켓 정리
    if (server_socket != -1) { // server_socket은 -1로 초기화되어 있으므로 0보다 큰지 확인
        close(server_socket);
        server_socket = -1; // 소켓 닫은 후 무효화
    }
    
    printf("채팅 프로그램을 종료합니다. 안녕히 가세요!\n");
}