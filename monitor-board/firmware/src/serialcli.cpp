#include "serialcli.h"

#include <stdlib.h>
#include <string.h>

#include "control.h"
#include "globals.h"
#include "serialstatemachine.h"

const int MAX_CMD_LEN = 31, MAX_TOKENS = 5;
byte cmd_buf[MAX_CMD_LEN+1];
byte* cmd_tokenv[MAX_TOKENS+1];
int cmd_len;

// States
static SerialState readingCommandState(byte ch);

// Process command stored in cmd_buf and return state to transition to.
static SerialState processCommand();

// Print a brief usage summary to the serial port.
static void printHelp();

// EXTERNAL

SerialState startSerialPrompt() {
    // Reset current command
    cmd_buf[0] = '\0';
    cmd_len = 0;

    Serial.print("> ");
    return { .next = &readingCommandState };
}

// INTERNAL

// State machine
static SerialState readingCommandState(byte ch) {
    switch(ch) {
        // handle backspace
        case 8:
        case 127:
            if(cmd_len > 0) {
                Serial.write(8);
                Serial.write(' ');
                Serial.write(8);
                cmd_len--;
                cmd_buf[cmd_len] = '\0';
            } else {
                // bell
                Serial.write(7);
            }
            break;

        // handle enter
        case 10:
        case 13:
            Serial.println("");
            return processCommand();
            break;

        default:
            // Only accept printable chars.
            if(ch < 0x20) {
                // Bell
                Serial.write(7);
            } else if(cmd_len < MAX_CMD_LEN) {
                // Add character to command buffer
                cmd_buf[cmd_len] = ch;
                cmd_len++;
                cmd_buf[cmd_len] = '\0';

                // Echo character to output
                Serial.write(ch);
            } else {
                // Bell
                Serial.write(7);
            }
            break;
    }

    return { .next = &readingCommandState };
}

static void printHelp() {
    Serial.println("?           - show brief help message");
    Serial.println("p[rint]     - print current address/data bus");
    Serial.println("h[alt]      - toggle halt state");
    Serial.println("c[ycle] [n] - single cycle n times");
    Serial.println("s[tep] [n]  - single step n times");
    Serial.println("reset       - toggle ~RST line");
    Serial.println("b[e]        - toggle BE line");
    Serial.println("");
    Serial.println("Specify decimal numbers with no prefix.");
    Serial.println("Specify hexadecimal numbers with $ prefix.");
}

// MODIFY cmd_buf tokenizing it into space-separated tokens skipping leading
// and trailing spaces. (Spaces are replaced with '\0'.) Update cmd_tokenv as
// pointers into cmd_buf for each token will NULL marking the end of the list.
static void tokenizeCmdBuf() {
    int next_tokenv_idx = 0;
    int ch_idx = 0;

    // Reset token vector
    cmd_tokenv[0] = NULL;

    while((next_tokenv_idx < MAX_TOKENS) && (ch_idx < cmd_len)) {
        // Skip to next non-space char
        for(; (ch_idx < cmd_len) && (cmd_buf[ch_idx] != '\0') &&
              (cmd_buf[ch_idx] == ' '); ++ch_idx) { }

        // Have we reached end of string without finding token?
        if((ch_idx == cmd_len) || (cmd_buf[ch_idx] == '\0')) {
            // yes, exit
            break;
        }

        // Record location of token and advance next_tokenv_idx
        cmd_tokenv[next_tokenv_idx] = &(cmd_buf[ch_idx]);
        ++next_tokenv_idx;

        // Start by assuming we won't find a token next time around
        cmd_tokenv[next_tokenv_idx] = NULL;

        // Advance to first space character or end of buffer
        for(;(ch_idx < cmd_len) && (cmd_buf[ch_idx] != '\0') &&
             (cmd_buf[ch_idx] != ' ')
            ;++ch_idx)
        { }

        // Write '\0' and advance ch_idx
        if(ch_idx < cmd_len) {
            cmd_buf[ch_idx] = '\0';
        }
        ++ch_idx;
    }

    // Reset command buffer length
    cmd_len = 0;

    // Ensure cmd_tokenv is terminated with a NULL come what may.
    cmd_tokenv[MAX_TOKENS] = NULL;
}

// return true if a == b or a == b[0]
static bool strprefixeq(const char* a, const char* b) {
    if((a[0] != '\0') && (a[0] == b[0]) && (a[1] == '\0')) {
        return true;
    }
    return !strcmp(a, b);
}

// Parse s as an integer and write result to l. Return true iff parsing
// succeeds.
bool parseLong(const char* s, long* l) {
    char* end_ptr;
    int base = 10;

    // Are we parsing a hex number?
    if(s[0] == '$') {
        base = 16;
        ++s;
    }

    *l = strtol(s, &end_ptr, base);
    return (s[0] != '\0') && (*end_ptr == '\0');
}

// Parse and perform a single cycle/instruction command.
static void performStepCommand(bool is_inst_step) {
    long n=1; // default
    if(cmd_tokenv[1] != NULL) {
        const char* arg1 = reinterpret_cast<const char*>(cmd_tokenv[1]);
        if(!parseLong(arg1, &n)) {
            Serial.print("invalid number: ");
            Serial.println(arg1);
        }
    }

    for(long i=0; i<n; ++i) {
        step_state = is_inst_step ? SS_INST : SS_CYCLE;
        while(step_state != SS_NONE) {
            controlLoop();

            if(!processorCanBeStepped()) {
                Serial.println("aborting: processor in incorrect state for stepping");
                step_state = SS_NONE;
            }
        }
    }
}

// Handle "addr (off | <address>)" command for asserting address bus.
void performAssertAddress() {
    const char* arg1 = reinterpret_cast<const char*>(cmd_tokenv[1]);
    if(strcmp(arg1, "off")) {
        // parse address value
        long v=0;
        if(!parseLong(arg1, &v)) {
            Serial.print("invalid address: ");
            Serial.println(arg1);
        }
        assert_address = true;
        out_address_bus = v;
    } else {
        // addr off - stop asserting
        assert_address = false;
    }
}

// Handle "data (off | <value>)" command for asserting data bus.
void performAssertData() {
    const char* arg1 = reinterpret_cast<const char*>(cmd_tokenv[1]);
    if(strcmp(arg1, "off")) {
        // parse address value
        long v=0;
        if(!parseLong(arg1, &v)) {
            Serial.print("invalid data: ");
            Serial.println(arg1);
        }
        assert_data = true;
        out_data_bus = v;
    } else {
        // data off - stop asserting
        assert_data = false;
    }
}

// perform the "write <addr> <val>" command
void performWrite() {
    const char* arg1 = reinterpret_cast<const char*>(cmd_tokenv[1]);
    const char* arg2 = reinterpret_cast<const char*>(cmd_tokenv[2]);
    long v;

    // Old state
    bool old_pull_be_low = pull_be_low;
    bool old_pull_rwbar_low = pull_rwbar_low;
    bool old_aa = assert_address, old_ad = assert_data;
    unsigned int old_a = out_address_bus;
    byte old_d = out_data_bus;

    if(!parseLong(arg1, &v)) {
        Serial.print("invalid address: ");
        Serial.println(arg1);
        return;
    }
    out_address_bus = v;

    if(!parseLong(arg2, &v)) {
        Serial.print("invalid data: ");
        Serial.println(arg1);
        return;
    }
    out_data_bus = v;

    // Drop BE
    pull_be_low = true;
    controlLoop();

    // Assert address
    assert_address = true;
    controlLoop();

    // Drop R/W~
    pull_rwbar_low = true;
    controlLoop();

    // Assert data
    assert_data = true;
    controlLoop();

    // Raise R/W~
    pull_rwbar_low = false;
    controlLoop();

    // Restore state
    pull_be_low = old_pull_be_low;
    pull_rwbar_low = old_pull_rwbar_low;
    assert_address = old_aa;
    assert_data = old_ad;
    out_address_bus = old_a;
    out_data_bus = old_d;
}

// perform the "read <addr>" command
void performRead() {
    const char* arg1 = reinterpret_cast<const char*>(cmd_tokenv[1]);
    long v;

    // Old state
    bool old_pull_be_low = pull_be_low;
    bool old_pull_rwbar_low = pull_rwbar_low;
    bool old_aa = assert_address, old_ad = assert_data;
    unsigned int old_a = out_address_bus;
    byte old_d = out_data_bus;

    if(!parseLong(arg1, &v)) {
        Serial.print("invalid address: ");
        Serial.println(arg1);
        return;
    }
    out_address_bus = v;

    // Drop BE
    pull_be_low = true;
    controlLoop();

    // Assert address
    assert_address = true;
    controlLoop();

    // Record data bus after one control loop
    controlLoop();
    byte d = data_bus;

    // Restore state
    pull_be_low = old_pull_be_low;
    pull_rwbar_low = old_pull_rwbar_low;
    assert_address = old_aa;
    assert_data = old_ad;
    out_address_bus = old_a;
    out_data_bus = old_d;

    Serial.print("D: ");
    Serial.print(d, HEX);
    Serial.println("");
}

static SerialState processCommand() {
    // Parse cmd_buf into tokens
    tokenizeCmdBuf();

    // Count tokens
    int n_tokens = 0;
    for(; (cmd_tokenv[n_tokens] != NULL) && (n_tokens < MAX_TOKENS)
        ; ++n_tokens) {}

    // Pull out command name
    const char *cmd = reinterpret_cast<const char*>(cmd_tokenv[0]);

    if(n_tokens == 0) {
        // If no command was given, print help
        printHelp();
    } else if(strprefixeq(cmd, "?") && (n_tokens == 1)) {
        // If help command was given, print help
        printHelp();
    } else if(strprefixeq(cmd, "halt") && (n_tokens == 1)) {
        halt = !halt;
        Serial.print("halt ");
        Serial.println(halt ? "on" : "off");
    } else if(strprefixeq(cmd, "print") && (n_tokens == 1)) {
        // print current state
        Serial.print("A: ");
        Serial.print(address_bus, HEX);
        Serial.print(" D: ");
        Serial.print(data_bus, HEX);
        Serial.println("");
    } else if(strprefixeq(cmd, "cycle") && (n_tokens <= 2)) {
        performStepCommand(false);
    } else if(strprefixeq(cmd, "step") && (n_tokens <= 2)) {
        performStepCommand(true);
    } else if(!strcmp(cmd, "reset") && (n_tokens == 1)) {
        pull_rst_low = !pull_rst_low;
        Serial.print("~rst ");
        Serial.println(pull_rst_low ? "low" : "high");
    } else if(strprefixeq(cmd, "be") && (n_tokens == 1)) {
        pull_be_low = !pull_be_low;
        Serial.print("be ");
        Serial.println(pull_be_low ? "low" : "high");
    } else if(strprefixeq(cmd, "rw") && (n_tokens == 1)) {
        // undocumented rw command
        pull_rwbar_low = !pull_rwbar_low;
        Serial.print("rwbar ");
        Serial.println(pull_rwbar_low ? "low" : "high");
    } else if(!strcmp(cmd, "addr") && (n_tokens == 2)) {
        // undocumented "addr" command for testing
        performAssertAddress();
    } else if(!strcmp(cmd, "data") && (n_tokens == 2)) {
        // undocumented "data" command for testing
        performAssertData();
    } else if(strprefixeq(cmd, "write") && (n_tokens == 3)) {
        performWrite();
    } else if(strprefixeq(cmd, "read") && (n_tokens == 2)) {
        performRead();
    } else {
        Serial.println("unknown command");
        printHelp();
    }

    return startSerialPrompt();
}
