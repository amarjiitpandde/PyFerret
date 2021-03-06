	SUBROUTINE CD_RD_EP_1(cdfid,t1id,t2id,t0,tunit,index,dval,status)

*
*
*  This software was developed by the Thermal Modeling and Analysis
*  Project(TMAP) of the National Oceanographic and Atmospheric
*  Administration's (NOAA) Pacific Marine Environmental Lab(PMEL),
*  hereafter referred to as NOAA/PMEL/TMAP.
*
*  Access and use of this software shall impose the following
*  obligations and understandings on the user. The user is granted the
*  right, without any fee or cost, to use, copy, modify, alter, enhance
*  and distribute this software, and any derivative works thereof, and
*  its supporting documentation for any purpose whatsoever, provided
*  that this entire notice appears in all copies of the software,
*  derivative works and supporting documentation.  Further, the user
*  agrees to credit NOAA/PMEL/TMAP in any publications that result from
*  the use of this software or in any product that includes this
*  software. The names TMAP, NOAA and/or PMEL, however, may not be used
*  in any advertising or publicity to endorse or promote any products
*  or commercial entity unless specific written permission is obtained
*  from NOAA/PMEL/TMAP. The user also understands that NOAA/PMEL/TMAP
*  is not obligated to provide the user with any support, consulting,
*  training or assistance of any kind with regard to the use, operation
*  and performance of this software nor to provide the user with any
*  updates, revisions, new versions or "bug fixes".
*
*  THIS SOFTWARE IS PROVIDED BY NOAA/PMEL/TMAP "AS IS" AND ANY EXPRESS
*  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
*  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
*  ARE DISCLAIMED. IN NO EVENT SHALL NOAA/PMEL/TMAP BE LIABLE FOR ANY SPECIAL,
*  INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
*  RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
*  CONTRACT, NEGLIGENCE OR OTHER TORTUOUS ACTION, ARISING OUT OF OR IN
*  CONNECTION WITH THE ACCESS, USE OR PERFORMANCE OF THIS SOFTWARE. 
*
*
* read a single time value from a 2-integer EPIC time axis
* and return it as a double precision time relative to t0

* Programmer Steve Hankin
* NOAA/PMEL, Seattle, WA - Tropical Modeling and Analysis Program

* revision 0.0 - 3/92
* V5.0 2/99 *sh* - corrected declaration error - "sec" is float
*		Note that fractional seconds are lost!
* V530 1/01 *acm* add calendar_id in TM_BREAK_DATE, TM_SECS_FROM_BC
*		  calendar is assumed Gregorian for epic data
* V65  *acm* 1/10  Change all netcdf2-style calls and parameter names to 
*                  netcdf3-style calls, which are also consistent with netcdf4.
* V65  *acm* 1/10  Add #ifdef usingDODSf2cUnderscore, for 32-bit linux build
*                  linked with OPeNDAP library.
* V66+ *acm* 4/10  Made incorrect call to NF_GET_VAR1_DOUBLE, should have becn
*                  NF_GET_VAR1_INT.
* V665 *acm* 11/10 Fix arguments to second call to NF_GET_VAR1_INT. 
* V6.74 3/12 *acm* cleanup ifdefs and unnecessary include files

* argument definitions:
*	cdfid	 - netCDF id number for already opened CDF file
*       t1id     - netCDF variable id of "time"
*       t2id     - netCDF variable id of "time2"
*       t0       - T0 string for the time axis
*       tunit    - number of seconds per axis time unit
*       index    - subscript of time axis to return
*       dval     - double precision value returned
*       status   - TMAP library return status

* argument declarations
      INTEGER cdfid, t1id, t2id, index, status
      REAL tunit
      REAL*8  dval
      CHARACTER*(*) t0

      include 'netcdf.inc'
      include 'tmap_errors.parm'

! Declare netcdf function names. 
! The ones without trailing underscore are declared in netcdf.inc
#include "netcdf_declareUnderscore.inc"

* internal variable declarations:
      INTEGER jday, msec, cdfstat, mon, day, yr, hour, min,
     .        t0year, t0mon, t0day, t0hour, t0min, t0sec
      INTEGER 	cal_id
	REAL	      sec					! 2/99
      REAL*8          TM_SECS_FROM_BC
      REAL*8          date_secs, start_secs, offset_secs

* break apart the T0 offset date.  Hardwire the calendar type to Gregorian.
      cal_id = 1
      CALL TM_BREAK_DATE (t0, cal_id, t0year, t0mon,
     .                      t0day, t0hour, t0min, t0sec, status)
      IF (status .NE. merr_ok) GO TO 5000

* read the 2 integer values
#ifdef usingDODSf2cUnderscore
      cdfstat = NF_GET_VAR1_INT_(cdfid, t1id, index, jday)

#else
      cdfstat = NF_GET_VAR1_INT (cdfid, t1id, index, jday)
#endif

      IF ( cdfstat .NE. NF_NOERR ) CALL TM_ERRMSG
     .     ( cdfstat+pcdferr, status, 'CD_RD_EP_TIME', cdfid, t1id,
     .       'EPIC time', no_errstring, *5000 )
#ifdef usingDODSf2cUnderscore
      cdfstat = NF_GET_VAR1_INT_(cdfid, t2id, index, msec)
#else
      cdfstat = NF_GET_VAR1_INT(cdfid, t2id, index, msec)
#endif

      IF ( cdfstat .NE. NF_NOERR ) CALL TM_ERRMSG
     .     ( cdfstat+pcdferr, status, 'CD_RD_EP_TIME', cdfid, t2id,
     .       'EPIC time2', no_errstring, *5000 )

* convert the 2 EPIC integers to a date
      CALL TM_EP_TIME_CONVRT( jday, msec,
     .                        mon, day, yr, hour, min, sec)

* determine seconds from BC for each part
      start_secs = TM_SECS_FROM_BC (cal_id, t0year, t0mon, t0day,
     .                              t0hour, t0min, t0sec,status)
      IF (status .NE. merr_ok) GO TO 5000
      date_secs  = TM_SECS_FROM_BC (cal_id, yr  , mon, day,
     .                              hour, min, NINT(sec),status)  ! NINT 2/99
      IF (status .NE. merr_ok) GO TO 5000

* Find the number of seconds since T0
      offset_secs = date_secs - start_secs

* Divide by time step increment to get # of time step
      dval = offset_secs/tunit

      status = merr_ok
      RETURN

* error exit
 5000 RETURN

      END


