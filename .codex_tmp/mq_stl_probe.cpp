#include <locale>
#include <sstream>

int main()
{
	std::stringstream stream;
	stream << 42;
	unsigned value = 0;
	stream >> value;
	return value == 42 ? 0 : 1;
}
