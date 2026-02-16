int g_counter = 5;

int c_expect_counter(int v) {
    return (g_counter == v) ? 0 : 1;
}
