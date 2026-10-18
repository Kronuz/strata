# Research prototypes for the indexing pipeline (docs/research/indexing.md). Built only with
# -DSTRATA_RESEARCH=ON; never part of the default build or CI. Each is a microbenchmark over
# the corpus sample, not engine code.
add_executable(research_bound bound.cc)
target_link_libraries(research_bound PRIVATE strata)
add_executable(research_invert invert.cc)
target_link_libraries(research_invert PRIVATE strata)
add_executable(research_order order.cc)
target_link_libraries(research_order PRIVATE strata)
add_executable(research_mergecost mergecost.cc)
target_link_libraries(research_mergecost PRIVATE strata)
