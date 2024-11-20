#include "transcode_with_filter.h"
#include "transcode_without_filter.h"
#include "transcode_dcm.h"

int main(int argc, char** argv)
{
	// With Filter
	transcode_with_filter();

	// Without Filter
	transcode_without_filter();

	// DICOM Converter
	transcode_dcm();
}