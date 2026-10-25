// The entry point of the aoo tool: kept separate from aoo.cpp because the
// Arduino headers declare a main() without arguments.
int aoo_main(int argc, char **argv);

int main(int argc, char **argv) { return aoo_main(argc, argv); }
