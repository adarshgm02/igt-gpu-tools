#include "DisplayPcDpst.h"

DD_DPST_ARGS args;

int main()	
{
printf("Checking Library Linkage");
args.Backlight_level = 10;
SetHistogramDataBin(args);
printf("Done Checking Library Linkage");
return 0;
}
