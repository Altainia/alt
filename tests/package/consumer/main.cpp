#include <alt/version.hpp>

// alt::version() is defined in src/version.cpp, so calling it makes the link pull a member
// out of the installed library rather than only naming it on the command line. A consumer
// that read nothing but the inline constants would link even against a library built in a
// configuration or runtime it cannot be combined with.
int main()
{
	return alt::version().empty() ? 1 : 0;
}
