#include "transcode.h"
#include "transcode_dcm.h"

int main(int argc, char** argv)
{
	// With Filter
	transcode_with_filter();

	// Without Filter
	transcode_without_filter();

	// DICOM Converter with Filter
	transcode_dcm_with_filter();

	// DICOM Converter without Filter
	transcode_dcm_without_filter();
}