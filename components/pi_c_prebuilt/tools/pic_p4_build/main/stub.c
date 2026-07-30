/* Stub entrypoint for the pi-c P4 library build project (never flashed).
 * The artefact we vendor is the component archive build/esp-idf/pi-c/libpi-c.a,
 * which IDF compiles whenever pi-c is in the build graph (main REQUIRES pi-c) —
 * regardless of whether this app references any pi-c symbol. Keep it empty so
 * the throwaway app always links. */
void app_main(void) {}
