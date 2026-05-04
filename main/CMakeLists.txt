idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    # Per-board pin/LED macros live at boards/<name>/board.h; the top-
    # level CMakeLists.txt resolves <name> from -D BOARD=... and stashes
    # the path in BOARD_DIR. main.c does #include "board.h".
    PRIV_INCLUDE_DIRS "${BOARD_DIR}"
    REQUIRES pt_app
)
