#include <pybind11/pybind11.h>
#include "atire_segment_index.h"
namespace py = pybind11;

PYBIND11_MODULE(_core, m) {
    m.attr("__doc__") = "Antelope engine binding (pybind11)";
    m.def("_link_check", []() {
        ATIRE_segment_index ix;   // must construct+destruct -> archive symbols must link
        (void)ix;
        return true;
    });
}
