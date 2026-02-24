#ifndef DFGERROR_H
#define DFGERROR_H

#include <stdexcept>
#include <string>

class DFGGenerationError : public std::runtime_error {
public:
    DFGGenerationError(const std::string &context, const std::string &msg)
        : std::runtime_error("DFG generation error in " + context + ": " + msg) {}
};

#define DFG_CHECK(cond, context, msg) \
    do { if (!(cond)) { throw DFGGenerationError(context, msg); } } while(0)

#endif
