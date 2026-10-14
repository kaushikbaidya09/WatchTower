# Embedded C Coding & Documentation Standard

Doxygen comments use `/*! ... */` blocks with indented content and no leading
`*` on each line, and the backslash command form (`\brief`, `\param`). Macro,
variable, struct member and enumerator comments use a trailing `///<`. No
`\details` sections and no author/date/version/copyright in file headers (Git
holds that), and no section-divider banners.

Based on common industry practice: **Barr Group Embedded C Coding Standard (BARR-C:2018)**,
**MISRA C:2012** guidelines, and **Doxygen** for documentation.

---

## 1. Project layout

```
WatchTower/
├── main/               main.c: entry point and display-request loop
├── components/         One ESP-IDF component per subsystem
│   ├── wt_app_led/     WS2812 render task, 7-segment display model, effects
│   ├── wt_app_log/     Logging (console + in-memory ring buffer)
│   ├── wt_app_settings/ User settings, stored in NVS
│   ├── wt_app_sound/   Buzzer driver
│   ├── wt_app_time/    DS3231 RTC driver
│   ├── wt_app_web/     HTTP + WebSocket server, serves the web console
│   └── wt_app_wifi/    WiFi, mDNS and NTP
├── web/                Web console, built into the firmware image
├── tools/              run.sh, format.sh, gzip_web.py
│   └── config/         .clang-format, .prettierrc, Doxyfile.in
├── simulation/         Mock web-console server and Wokwi project
├── partitions/         Partition table
├── docs/               Design, getting started, this standard
├── tests/              PC-side unit tests (Unity / CMock / Ceedling);
│                       create it with your first test
├── CMakeLists.txt
└── version.txt         Project version (Semantic Versioning)
```

Each component keeps its public headers in `include/` and its sources beside
its own `CMakeLists.txt`. Layering (upper layers may call lower ones, never
the reverse):

```
  main.c                Entry point, display-request loop
  wt_app_<name>         Components; dependencies are the REQUIRES lists in
                        each component's CMakeLists.txt
  ESP-IDF / FreeRTOS    Supplied by Espressif
```

The dependency graph between components is in `docs/DESIGN.md`, section 3.4.

---

## 2. File header (every .c and .h)

```c
//=============================================================================
/*!
    \file    uart.c
    \brief   One-line summary.
*/
//=============================================================================
```

The file header is framed as one block by a `//===…` single-line comment (79
characters) directly above and below the `/*! */` block; it contains only
`\file` and `\brief` (author, date and version live in Git). The border lines
are ordinary comments, not part of the Doxygen block.

---

## 3. Section order inside a file

**.c file**
1. File header
2. Includes — own header first, then project headers, then system headers
3. Private macros / constants
4. Private types
5. Private (static) variables
6. Private function prototypes
7. Public function definitions
8. Interrupt service routines
9. Private function definitions

**.h file**
1. File header
2. Include guard `#ifndef MODULE_H`
3. Includes (only what the header needs)
4. Public macros / constants
5. Public types
6. Public `extern` variables (avoid)
7. Public function prototypes
8. Close include guard


---

## 4. Function header

Function documentation goes **only on the definition in the .c file** — header
prototypes carry no comment block, so there is one place to keep up to date.

**Public functions** — `\brief`, every `\param` with direction, and `\return`
(if non-void). Nothing else.

```c
/*!
    \brief   Read one sample from the sensor.
    \param[in]  p_cfg    Configuration. Must not be NULL.
    \param[out] p_value  Receives the sample.
    \return  UART_OK on success, UART_ERR_PARAM if an argument is invalid.
*/
```

**Static (private) functions and ISRs** — `\brief` only.

```c
/*!
    \brief   Write an LED output and cache its state.
*/
static void led_write(led_ctrl_t *p_led, bool b_on)
```

Put anything else worth saying (reentrancy, ISR-safety, preconditions) as a
`//` comment inside the function body.

---

## 5. Inline comments

| Use | Format |
|---|---|
| Explain *why*, not *what* | `// Delay lets the PLL settle before switching source.` |
| Macros, variables, members, enumerators | `#define UART_BUF_SIZE (64U)  ///< RX buffer size in bytes.` |
| Work to do | `// TODO: <what>` |
| Known defect / hack | `// FIXME: <issue id> <what>` |
| Datasheet reference | `// RM0090 §30.6.4: BRR = fCK / baud` |
| MISRA deviation | `// MISRA 11.4 deviation DEV-007: register address cast` |

Rules:
- Comments inside function bodies use single-line `//`, never `/* */`.
- `///<` is used only for documenting macros, variables, members and enumerators.
- Both `//` and `///<` require C99 or later.
- Never comment out code — delete it; version control keeps it.
- Comment every magic number or replace it with a named constant.
- Keep comments correct when code changes; a wrong comment is worse than none.

---

## 6. Naming

| Item | Convention | Example |
|---|---|---|
| Files | lower_snake, module name | `uart_drv.c`, `uart_drv.h` |
| Public functions | `module_verb_object()` | `uart_send_byte()` |
| Private functions | `static`, same style | `static void uart_flush_fifo(void)` |
| Variables | lower_snake | `rx_count` |
| File-scope (static) globals | `g_` prefix | `g_rx_count` |
| Pointers | `p_` prefix | `p_buffer` |
| Booleans | `b_` prefix | `b_ready` |
| Macros / constants | UPPER_SNAKE, module prefix | `UART_BUF_SIZE` |
| Types | `module_name_t` | `uart_config_t` |
| Enum members | UPPER_SNAKE, module prefix | `UART_ERR_TIMEOUT` |
| ISRs | match vector table | `USART1_IRQHandler` |

---

## 7. Coding rules (MISRA / BARR-C essentials)

**Types**
- Use `<stdint.h>` fixed-width types (`uint8_t`, `int32_t`) — never bare `int`/`long` for data.
- Use `<stdbool.h>` `bool`.
- Unsigned literal suffix: `10U`, `1000UL`.
- No implicit narrowing; cast explicitly: `(uint8_t)(value & 0xFFU)`.

**Structure**
- Always use braces, even for single statements.
- One exit point per function (single `return` at the end).
- Every `switch` has a `default`; every `case` ends with `break`.
- Every `if … else if` chain ends with `else`.
- Constant on the left in comparisons: `if (NULL == p)`.
- Cast ignored return values to `(void)`.

**Memory & safety**
- No dynamic allocation (`malloc`/`free`) after init — preferably never.
- No recursion.
- Validate all pointer and range parameters of public functions.
- All hardware waits must be bounded (timeout counters).
- File-scope data is `static`; minimise globals.

**Concurrency**
- Variables shared with ISRs are `volatile`.
- Multi-byte shared data on 8/16-bit MCUs is accessed inside a critical section.
- ISRs are short: clear flag, copy data, set flag/queue, return.
- No blocking calls, `printf`, or floating point in ISRs.

**Preprocessor**
- Parenthesise macro bodies and parameters: `#define SQR(x) ((x) * (x))`.
- Prefer `static inline` functions and `enum`/`const` over function-like macros.
- Include guards in every header.

---

## 8. Formatting

- 4-space indent, no tabs. Max line length 100 chars.
- Allman braces (brace on its own line).
- One declaration per line.
- Space after keywords: `if (`, `for (`, `while (`.
- Enforce automatically with `clang-format` (see `tools/config/.clang-format`); run `tools/format.sh`. The build runs it automatically.

---

## 9. Tooling

| Purpose | Tool |
|---|---|
| Documentation | Doxygen (`docs` CMake target) |
| Static analysis | cppcheck (`check` CMake target), PC-lint Plus, Polyspace, Parasoft |
| MISRA checking | cppcheck `--addon=misra`, PC-lint Plus, LDRA |
| Formatting | clang-format for C, Prettier (`tools/config/.prettierrc`) for `web/`; both via `tools/format.sh` or the `format` CMake target |
| Unit tests | Unity, CMock, Ceedling |
| Warnings | `-Wall -Wextra -Werror -Wconversion -Wshadow` |

---

## 10. Versioning

- The project has one version, in `version.txt`, in Semantic Versioning format:
  `MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]` (see semver.org). The build rejects
  anything else and embeds it in the firmware image as ESP-IDF's `PROJECT_VER`.
- Modules do not carry their own version numbers.

---

## 11. New module checklist

1. Put the header in the component's `include/` folder and the source beside
   its `CMakeLists.txt` (or create a new component under `components/`), and
   add the `.c` file to the component's `SRCS`. Start both files with the
   header from section 2.
2. Keep the section order of section 3; delete the parts you do not need.
3. Every public and private name carries the module prefix (section 6).
4. Every public function validates its parameters and has a full function
   header; static functions and ISRs have `\brief` only (section 4).
5. The build has no warnings, `tools/format.sh --check` passes, and the `check`
   and `docs` targets are clean.
