#pragma once

#define MAX_DEFINE_TOKENS 100
#define MAX_DEFINE_RECURSION_DEPTH 10
#define MAX_DEFINE_TOKENS_COUNTER_INIT int defined_tokens_count = 0;
#define IF_DEFINE_TOKEN_COUNT_IS_ZERO(body) if (defined_tokens_count == 0) {\
                                                 body                        \
                                             }
#define IF_DEFINE_TOKEN_COUNT_MORE_MAX_TOKENS(body) if (defined_tokens_count > MAX_DEFINE_TOKENS) {\
                                                        body                                       \
                                                    }

#define MAX_MACRO_TOKENS 1000
#define MAX_MACRO_ARGS 10
#define MAX_MACRO_RECURSION_DEPTH 10

// unsafe macro
// This macro marks a function as unsafe, meaning
// that is function can cause undefined behavior.
#define unsafe [[deprecated("use unsafe function instead")]]

// default debug macro
// This macro is used for debugging purposes.
#define DEBUG if (debug_mode)

#define MSG(v) std:: cout << v << std:: endl;
#define TEST MSG("TEST")

#define ANALYZER_DELAY 0.2
#define DELAY(x) std::this_thread::sleep_for(std::chrono::milliseconds(x))


// Messages for preprocessor syntax errors
#define NOTE_INCLUDE_SYNTAX "Include directive syntax: #include <\"file_path\">"
#define NOTE_DEFINE_SYNTAX "Define directive syntax: #define <name> = <sequence of tokens>;"
#define NOTE_MACRO_SYNTAX "Macro directive syntax: #macro <name>(args, ...) { <sequence of tokens> }"
#define NOTE_UNDEF_SYNTAX "Undef directive syntax: #undef <name>"
#define NOTE_TOKEN_PASTING_SYNTAX "Token pasting syntax: <identifier>$$<identifier> or <identifier>$$<integer>"
#define NOTE_STRINGIFY_SYNTAX "Stringify syntax: @<token> or @(<tokens>)"
