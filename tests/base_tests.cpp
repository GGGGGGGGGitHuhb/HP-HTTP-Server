#include "base/logger.h"
#include "base/non_copyable.h"

#include <type_traits>
#include <utility>

namespace {

class ExampleResource : private hp::base::NonCopyable {
public:
    ExampleResource() = default;
    ExampleResource(ExampleResource&&) noexcept = default;
    ExampleResource& operator=(ExampleResource&&) noexcept = default;
};

static_assert(!std::is_copy_constructible_v<ExampleResource>);
static_assert(!std::is_copy_assignable_v<ExampleResource>);
static_assert(std::is_nothrow_move_constructible_v<ExampleResource>);
static_assert(std::is_nothrow_move_assignable_v<ExampleResource>);

}  // namespace

int main() {
    hp::base::info("base test info");
    hp::base::warn("base test warning");
    hp::base::error("base test error");

    ExampleResource source;
    ExampleResource destination(std::move(source));
    source = std::move(destination);
    return 0;
}
