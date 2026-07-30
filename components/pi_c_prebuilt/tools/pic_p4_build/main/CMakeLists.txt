# Stub app component: exists only so the build graph reaches pi-c. Its
# REQUIRES pulls pi-c (and its transitive deps) into the build; we then grab
# build/esp-idf/pi-c/libpi-c.a. Nothing here is ever executed.
idf_component_register(SRCS "stub.c" REQUIRES pi-c)
