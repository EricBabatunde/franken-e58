# Pure C++ (no IDF headers), so tools/host_test/test_safety.cpp builds the same sources on the
# laptop. linker.lf places the whole component in IRAM: it is on the Core 1 control path.
idf_component_register(
    SRCS "safety.cpp" "rest.cpp"
    INCLUDE_DIRS "include"
    LDFRAGMENTS "linker.lf")
