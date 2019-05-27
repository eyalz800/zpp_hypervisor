#pragma once
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace zpp
{
/**
 * Returns the error category for a given error code enumeration type,
 * using an argument dependent lookup of a user implemented category
 * function.
 */
template <typename ErrorCode>
decltype(auto) category()
{
    return category(ErrorCode{});
}

/**
 * The error category which responsible for translating error codes to
 * error messages.
 */
class error_category
{
public:
    /**
     * Returns the error category name.
     */
    virtual std::string_view name() const noexcept = 0;

    /**
     * Return the error message for a given error code.
     * For success codes, it is unspecified what value is returned.
     * For convienience, you may return zpp::error::no_error for success.
     * All other codes must return non empty string views.
     */
    virtual std::string_view message(int code) const noexcept = 0;

    /**
     * Returns true if success code, else false.
     */
    bool success(int code) const
    {
        return code == m_success_code;
    }

protected:
    /**
     * Creates an error category whose success code is 'success_code'.
     */
    constexpr error_category(int success_code) :
        m_success_code(success_code)
    {
    }

    /**
     * Destroys the error category.
     */
    ~error_category() = default;

private:
    /**
     * The success code.
     */
    int m_success_code{};
};

/**
 * Creates an error category, whose name and success
 * code are specified, as well as a message translation
 * logic that returns the error message for every error code.
 * Note: message translation must not throw.
 */
template <typename ErrorCode, typename Messages>
constexpr auto make_error_category(std::string_view name,
                                   ErrorCode success_code,
                                   Messages && messages)
{
    // Create a category with the name and messages.
    class category : public error_category,
                     private std::remove_reference_t<Messages>
    {
    public:
        constexpr category(std::string_view name,
                           ErrorCode success_code,
                           Messages && messages) :
            error_category(
                std::underlying_type_t<ErrorCode>(success_code)),
            std::remove_reference_t<Messages>(
                std::forward<Messages>(messages)),
            m_name(name)
        {
        }

        std::string_view name() const noexcept override
        {
            return m_name;
        }

        std::string_view message(int code) const noexcept override
        {
            return this->operator()(ErrorCode{code});
        }

    private:
        std::string_view m_name;
    } category(name, success_code, std::forward<Messages>(messages));

    // Return the category.
    return category;
}

/**
 * This namespace is a workaround allowing us to delay the introduction of
 * 'error' in this scope to avoid conflict with language rules in class
 * 'maybe'.
 */
namespace error_detail
{
/**
 * Represents an error to be initialized from an error code
 * enumeration.
 * The error code enumeration must have 'int' as underlying type.
 * Defining an error code enum and a category for it goes as follows.
 * Example:
 * ~~~
 * namespace my_namespace
 * {
 * enum class my_error : int
 * {
 *     success = 0,
 *     something_bad = 1,
 *     something_really_bad = 2,
 * };
 *
 * inline const zpp::error_category & category(my_error)
 * {
 *     constexpr static auto error_category =
 *         zpp::make_error_category("my_category", my_error::success,
 *             [](auto code) -> std::string_view {
 *                 switch (code) {
 *                     case my_error::success:
 *                         return zpp::error::no_error;
 *                     case my_code:something_bad:
 *                         return "Something bad happened.";
 *                     case my_error::something_really_bad:
 *                         return "Something really bad happened.";
 *                     default:
 *                         return "Unknown error occurred.";
 *                 }
 *             }
 *         );
 *     return error_category;
 * }
 * } // my_namespace
 * ~~~
 */
class error
{
public:
    /**
     * Disables default construction.
     */
    error() = delete;

    /**
     * Constructs an error from an error code enumeration, the
     * category is looked by using argument dependent lookup on a
     * function named 'category' that receives the error code
     * enumeration value.
     */
    template <typename ErrorCode>
    error(ErrorCode error_code) :
        m_category(std::addressof(zpp::category<ErrorCode>())),
        m_code(std::underlying_type_t<ErrorCode>(error_code))
    {
    }

    /**
     * Constructs an error from an error code enumeration, the
     * category is given explicitly in this overload.
     */
    template <typename ErrorCode>
    error(ErrorCode error_code, const error_category & category) :
        m_category(std::addressof(category)),
        m_code(std::underlying_type_t<ErrorCode>(error_code))
    {
    }

    /**
     * Returns the error category.
     */
    const error_category & category() const
    {
        return *m_category;
    }

    /**
     * Returns the error code.
     */
    int code() const
    {
        return m_code;
    }

    /**
     * Returns the error message. Calling this on
     * a success error is implementation defined according
     * to the error category.
     */
    std::string_view message() const
    {
        return m_category->message(m_code);
    }

    /**
     * Returns true if the error indicates success, else false.
     */
    explicit operator bool() const
    {
        return m_category->success(m_code);
    }

    /**
     * No error message value.
     */
    static constexpr std::string_view no_error{};

private:
    /**
     * The error category.
     */
    const error_category * m_category{};

    /**
     * The error code.
     */
    int m_code{};
};
} // namespace error_detail

/**
 * Represents a value-or-error object.
 * An object of this type may have a value stored inside or an error.
 * The error is represented as zpp::error, and can be queried accessed
 * using the 'error' member function.
 * Example:
 * ~~~
 * // After defining my_error and the category of it as described
 * // in the sample above for the error class.
 *
 * // Change this value to observe different behavior.
 * bool is_success = true;
 *
 * // Example of using 'maybe'.
 * zpp::maybe<int> foo(bool value)
 * {
 *     if (!value) {
 *         // Fail with error code.
 *         return my_error::something_bad;
 *     }
 *
 *     // Success
 *     return 1337;
 * }
 *
 * // Example of using plain error code.
 * zpp::error bar(bool value)
 * {
 *     if (!value) {
 *         return my_error::something_really_bad;
 *     }
 *
 *     return my_error::success;
 * }
 *
 * // Call foo and check error.
 * void baz1()
 * {
 *     if (auto result = foo(is_success)) {
 *         // Success path.
 *         std::cout << "Success path: value is '"
 *             << result.value() << "'\n";
 *     } else {
 *         // Error path.
 *         std::cout << "Error path: error is '"
 *             << result.error().code() << "', '"
 *             << result.error().message() << "'\n";
 *     }
 * }
 *
 * // Call bar and check error.
 * void baz2()
 * {
 *     if (auto error = bar(is_success); !error) {
 *         // Error path.
 *         std::cout << "Error path: error is '"
 *             << error.code() << "', '"
 *             << error.message() << "'\n";
 *     } else {
 *         // Success path.
 *         std::cout << "Success path: code is '"
 *             << error.code() << "'\n";
 *     }
 * }
 * ~~~
 */
template <typename Type>
class maybe : private std::variant<Type, error_detail::error>
{
public:
    /**
     * The base class.
     */
    using base = std::variant<Type, error_detail::error>;

    /**
     * The type of value.
     */
    using type = Type;

    /**
     * Alias to the error type.
     */
    using error_type = error_detail::error;

    /**
     * Disable default construction.
     */
    maybe() = delete;

    /**
     * Inherit base class constructors.
     */
    using std::variant<Type, error_type>::variant;

    /**
     * Returns the stored error.
     * The behavior is undefined if the object has a stored value.
     */
    auto error() const
    {
        return *std::get_if<error_type>(this);
    }

    /**
     * Returns the stored value.
     * The behavior is undefined if the object has a stored error.
     */
    decltype(auto) value() &&
    {
        return std::move(*std::get_if<type>(this));
    }

    /**
     * Returns the stored value.
     * The behavior is undefined if the object has a stored error.
     */
    decltype(auto) value() &
    {
        return *std::get_if<type>(this);
    }

    /**
     * Returns false if there is a stored error, else, there
     * is a stored value and the return value is true.
     */
    explicit operator bool() const
    {
        return (0 == this->index());
    }
};

/**
 * Introduce error here instead of before 'maybe'.
 */
using error = error_detail::error;

} // namespace zpp
