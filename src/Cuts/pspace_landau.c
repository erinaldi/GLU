/*
    Copyright 2013 Renwick James Hudspith

    This file (pspace_landau.c) is part of GLU.

    GLU is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    GLU is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with GLU.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
   @file pspace_landau.c
   @brief momentum space landau condition corrections
 */

#include "Mainfile.h"
#include "geometry.h" // for the 0 -> 2Pi to -Pi -> Pi BZ calc

// momentum space landau factor
// writing not the whole lattice
void
correct_pspace_landau( struct site *__restrict A ,
		       const struct veclist *__restrict list ,
		       const int *__restrict in ,
		       const int DIMS )
{
  double ave_err = 0. ;
  int i ;
  #pragma omp parallel for private(i) reduction(+:ave_err)
  PFOR( i = 0 ; i < in[0] ; i++ ) {

    const int list_idx = list[ i ].idx ;

    // this is for the p-space test
    double complex sum[ NCNC ] = { } ;

    // Loop over polarisations ...
    int mu ;
    for( mu = 0 ; mu < DIMS ; mu++ ) {

      const double cache = 0.5 * list[i].MOM[ mu ] * Latt.twiddles[ mu ] ;
      const double p_mu = 2.0 * sin( cache ) ;
 
      #if ( defined deriv_linn ) || ( defined deriv_fulln )
      const double p_mu_nn = 2.0 * ( nn1 * sin( cache ) + nn2 * sin( cache * 3.0 ) ) ;
      #elif defined deriv_fullnn
      const double p_mu_nn = 2.0 * ( nnn1 * sin( cache ) + nnn2 * sin( cache * 3.0 ) + nnn3 * sin( cache * 5.0 ) ) ;
      #endif

      // FFTWS forward cut has the conventional e^{-ipx}
      #ifdef CUT_FORWARD
      const double complex exp_cache = cos( cache ) - I * 0.5 * p_mu ; // e^{-ip_\mu / 2 }
      #else
      const double complex exp_cache = cos( cache ) + I * 0.5 * p_mu ; // e^{ip_\mu / 2 }
      #endif
      
      // write over every element of A with this extra factor
      int j ;
      for( j = 0 ; j < NCNC ; j++ ) {
	A[ list_idx ].O[ mu ][ j ] *= exp_cache ;
        #if ( defined deriv_linn ) || ( defined deriv_fulln ) || ( defined deriv_fullnn )
	sum[ j ] += (double complex)A[ list_idx ].O[ mu ][ j ] * p_mu_nn ;
	#else
	sum[ j ] += (double complex)A[ list_idx ].O[ mu ][ j ] * p_mu ;
	#endif
      }
    }
    // and finally do the reduction on ave_err
    double tmp = 0. ;
    int j ;
    for( j = 0 ; j < NCNC ; j++ ) {
      tmp += cabs( sum[j] ) ;
    }
    ave_err = ave_err + (double)( tmp / (double)NCNC ) ;
  }
  printf( "\n[CUTS] P-Space gauge fixing error :: %1.5e \n\n" , ave_err / (double)in[0] ) ;
  return ;
}
