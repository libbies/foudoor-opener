#include <WiFi.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <libssh_esp32.h>
#include <libssh/libssh.h>

#define BGCOLOR TFT_BLACK
#define FGCOLOR TFT_WHITE

const char* ssid = "ssid";
const char* ssid_password = "password";
const char* ssh_host = "foudoor.local";
const char* ssh_user = "catties";
const char* SSH_PRIV_KEY_FILE = "/.ssh/id_ed25519_foudoor";
ssh_key ssh_priv_key = NULL;

// M5Cardputer setup
M5Canvas canvas(&M5Cardputer.Display);
String commandBuffer = "";
int cursorY = 0;
const int lineHeight = 32;
unsigned long lastKeyPressMillis = 0;
const unsigned long debounceDelay = 150;

String readUserInput(bool isYesNoInput = false) {
    String input = "";
    bool inputComplete = false;

    while (!inputComplete) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.isChange()) {
            if (M5Cardputer.Keyboard.isPressed()) {
                Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
                for (auto i : status.word) {
                    input += i;
                    M5Cardputer.Display.print(i);
                }

                if (status.del && !input.isEmpty()) {
                    input.remove(input.length() - 1);
                    M5Cardputer.Display.setCursor(M5Cardputer.Display.getCursorX() - 6, M5Cardputer.Display.getCursorY());
                    M5Cardputer.Display.print(" "); // Print a space to erase the last character
                    M5Cardputer.Display.setCursor(M5Cardputer.Display.getCursorX() - 6, M5Cardputer.Display.getCursorY());
                }

                if (status.enter || (isYesNoInput && (input == "Y" || input == "y" || input == "N" || input == "n"))) {
                    inputComplete = true;
                }
            }
        }
    }
    return input;
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextSize(1); // Set text size

    float batPercent = M5.Power.getBatteryLevel();
    float batVoltage = M5.Power.getBatteryVoltage();
    M5Cardputer.Display.printf(
            "door-opener by catties ()\nbattery: %.2f %%  (%.2f v)\n",
            batPercent,
            batVoltage
        );

    if (!SD.begin(SS)) {
        M5Cardputer.Display.println("error: failed to mount SD card file system.");
        return;
    }

    // Connect to WiFi
    WiFi.begin(ssid, ssid_password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }
    M5Cardputer.Display.printf("\nconnected to SSID: %s\n", ssid);

    // Prompt SSH setup
    M5Cardputer.Display.print("\nunlock foudoor? (y/n): ");
    String unlock = readUserInput(true);

    if (!(unlock == "Y" || unlock == "y")) {
        esp_restart();
    } else {
        File f = SD.open(SSH_PRIV_KEY_FILE, FILE_READ);
        if (!f) {
            M5Cardputer.Display.println("\nfailed to read privkey from SD!");
        } else {
            String keyData = f.readString();
            M5Cardputer.Display.printf("\nread %d bytes from SD for privkey", keyData.length());
            f.close();
            int rc = ssh_pki_import_privkey_base64(keyData.c_str(), NULL, NULL, NULL, &ssh_priv_key);
            M5Cardputer.Display.printf("\nimport privkey rc = %d", rc);
        }
    }

    // Connect to SSH server
    TaskHandle_t sshTaskHandle = NULL;
    xTaskCreatePinnedToCore(sshTask, "SSH Task", 20000, NULL, 1, &sshTaskHandle, 1);
    if (sshTaskHandle == NULL) {
        M5Cardputer.Display.println("\nfailed to create SSH task");
    }

    // Initialize the cursor Y position
    cursorY = M5Cardputer.Display.getCursorY();
}

void loop() {
    M5Cardputer.update();
}

ssh_session connect_ssh(const char *host, const char *user, int verbosity) {
    ssh_session session = ssh_new();
    if (session == NULL) {
        M5Cardputer.Display.println("\nFailed to create SSH session");
        return NULL;
    }

    ssh_options_set(session, SSH_OPTIONS_HOST, host);
    ssh_options_set(session, SSH_OPTIONS_USER, user);
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);

    if (ssh_connect(session) != SSH_OK) {
        Serial.print("\nerror connecting to host: ");
        Serial.println(ssh_get_error(session));
        ssh_free(session);
        return NULL;
    }

    return session;
}

int authenticate_console(ssh_session session) {
    //int rc = ssh_userauth_password(session, NULL, password);
    int rc = ssh_userauth_publickey(session, NULL, ssh_priv_key);
    if (rc != SSH_AUTH_SUCCESS) {
        M5Cardputer.Display.println("\nerror authenticating with privkey: ");
        M5Cardputer.Display.println(ssh_get_error(session));
        return rc;
    }
    return SSH_OK;
}

void sshTask(void *pvParameters) {
    ssh_session my_ssh_session = connect_ssh(ssh_host, ssh_user, SSH_LOG_PROTOCOL);
    if (my_ssh_session == NULL) {
        M5Cardputer.Display.println("\nSSH connection failed.");
        vTaskDelete(NULL);
        return;
    }

    M5Cardputer.Display.println("\nSSH connection established.");
    if (authenticate_console(my_ssh_session) != SSH_OK) {
        M5Cardputer.Display.println("\nSSH authentication failed.");
        ssh_disconnect(my_ssh_session);
        ssh_free(my_ssh_session);
        vTaskDelete(NULL);
        return;
    }

    M5Cardputer.Display.println("\nSSH authentication succeeded.");

    // Open a new channel for the SSH session
    ssh_channel channel = ssh_channel_new(my_ssh_session);
    if (channel == NULL || ssh_channel_open_session(channel) != SSH_OK) {
        M5Cardputer.Display.println("\nSSH channel open error.");
        ssh_disconnect(my_ssh_session);
        ssh_free(my_ssh_session);
        vTaskDelete(NULL);
        return;
    }

    // Request a pseudo-terminal
    if (ssh_channel_request_pty(channel) != SSH_OK) {
        M5Cardputer.Display.println("\nrequest PTY failed.");
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(my_ssh_session);
        ssh_free(my_ssh_session);
        vTaskDelete(NULL);
        return;
    }

    // Start a shell session
    if (ssh_channel_request_shell(channel) != SSH_OK) {
        M5Cardputer.Display.println("\nrequest shell failed.");
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(my_ssh_session);
        ssh_free(my_ssh_session);
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        M5Cardputer.update();

        // Handle keyboard input with debounce
        if (M5Cardputer.Keyboard.isChange()) {
            if (M5Cardputer.Keyboard.isPressed()) {
                unsigned long currentMillis = millis();
                if (currentMillis - lastKeyPressMillis >= debounceDelay) {
                    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

                    for (auto i : status.word) {
                        commandBuffer += i;
                        M5Cardputer.Display.print(i); // Display the character as it's typed
                        cursorY = M5Cardputer.Display.getCursorY(); // Update cursor Y position
                    }

                    if (status.del && commandBuffer.length() > 2) {
                        commandBuffer.remove(commandBuffer.length() - 1);
                        M5Cardputer.Display.setCursor(M5Cardputer.Display.getCursorX() - 6, M5Cardputer.Display.getCursorY());
                        M5Cardputer.Display.print(" "); // Print a space to erase the last character
                        M5Cardputer.Display.setCursor(M5Cardputer.Display.getCursorX() - 6, M5Cardputer.Display.getCursorY());
                        cursorY = M5Cardputer.Display.getCursorY(); // Update cursor Y position
                    }

                    if (status.enter) {
                        String message = commandBuffer.substring(2) + "\r\n"; // Use "\r\n" for newline
                        ssh_channel_write(channel, message.c_str(), message.length()); // Send message to SSH server

                        commandBuffer = "> ";
                        M5Cardputer.Display.print('\n'); // Move to the next line
                        cursorY = M5Cardputer.Display.getCursorY(); // Update cursor Y position
                    }

                    lastKeyPressMillis = currentMillis;
                }
            }
        }

        // Check if the cursor has reached the bottom of the display
        if (cursorY > M5Cardputer.Display.height() - lineHeight) {
            // Scroll the display up by one line
            M5Cardputer.Display.scroll(0, -lineHeight);

            // Reset the cursor to the new line position
            cursorY -= lineHeight;
            M5Cardputer.Display.setCursor(M5Cardputer.Display.getCursorX(), cursorY);
        }

        char buffer[1024];
        int nbytes = ssh_channel_read_nonblocking(channel, buffer, sizeof(buffer), 0);
        if (nbytes > 0) {
            for (int i = 0; i < nbytes; ++i) {
                if (buffer[i] == '\r') {
                    continue; // Handle carriage return
                }
                M5Cardputer.Display.write(buffer[i]);
                cursorY = M5Cardputer.Display.getCursorY(); // Update cursor Y position
            }
        }

        if (nbytes < 0 || ssh_channel_is_closed(channel)) {
            break;
        }
    }

    // Clean up
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    ssh_disconnect(my_ssh_session);
    ssh_free(my_ssh_session);
    vTaskDelete(NULL);
}
