// strata's key-value driver: kv_strata.hh's engine under kvdriver.hh's rows.
#include "kv_strata.hh"

int main(int argc, char** argv) { return kvbench::main_of<strata_kv>(argc, argv); }
