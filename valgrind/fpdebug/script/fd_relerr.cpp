#include <stdio.h>
#include <gmp.h>
#include <mpfr.h>
#include <fstream>
#include <string>
#include <stdlib.h>
#include <string.h>

#define PREC 120

using namespace std;

char* mpfrToStringE(char* str, mpfr_t* fp) {
	int sgn = mpfr_sgn(*fp);
	if (sgn >= 0) {
		str[0] = ' '; str[1] = '0'; str[2] = '\0';
	} else {
		str[0] = '-'; str[1] = '\0';
	}

	char mpfr_str[100]; /* digits + 1 */
	mpfr_exp_t exp;
	/* digits_base10 = log10 ( 2^(significant bits) ) */
	mpfr_get_str(mpfr_str, &exp, /* base */ 10, /* digits, float: 7, double: 15 */ 60, *fp, MPFR_RNDN);
	//mpfr_get_str(mpfr_str, &exp, /* base */ 10, /* digits, float: 7, double: 15 */ 60, *fp, STD_RND);
	exp--;
	strcat(str, mpfr_str);

	if (sgn >= 0) {
		str[1] = str[2];
		str[2] = '.';
	} else {
		str[1] = str[2];
		str[2] = '.';
	}

	char exp_str[50];
	sprintf(exp_str, " * 10^%ld", exp);
	strcat(str, exp_str);
	return str;
}

int main(int argc, char const *argv[]) {
	ifstream shin("shadow.fd.temp");
	ifstream orin("original.fd.temp");
	string line;
	mpfr_t sv, ov;
	mpfr_init2(sv, PREC);
	mpfr_init2(ov, PREC);
	while(shin >> line) {
		if (line == "SHADOW") {
			shin >> line; // value:
			shin >> line;
			mpfr_set_str(sv, line.c_str(), 10, MPFR_RNDN);
			break;
		}
	}
	while(orin >> line) {
		if (line == "ORIGINAL") {
			orin >> line; // value:
			orin >> line;
			mpfr_set_str(ov, line.c_str(), 10, MPFR_RNDN);
			break; 
		}
	}
	shin.close();
	orin.close();
	mpfr_t rel;
	mpfr_init2(rel, PREC);
	if (mpfr_cmp_ui(sv, 0) != 0 || mpfr_cmp_ui(ov, 0) != 0) {
		mpfr_reldiff(rel, sv, ov, MPFR_RNDN);
		mpfr_abs(rel, rel, MPFR_RNDN);
	} else {
		mpfr_set_ui(rel, 0, MPFR_RNDN);
	}
	char mpfrBuf[200];
	mpfrToStringE(mpfrBuf, &rel);
	ofstream out("fpdebug_relerr.log");
	out << mpfrBuf << endl;
	out.close();
	return 0;
}