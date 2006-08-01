/***************************Copyright-DO-NOT-REMOVE-THIS-LINE**
  *
  * Condor Software Copyright Notice
  * Copyright (C) 1990-2006, Condor Team, Computer Sciences Department,
  * University of Wisconsin-Madison, WI.
  *
  * This source code is covered by the Condor Public License, which can
  * be found in the accompanying LICENSE.TXT file, or online at
  * www.condorproject.org.
  *
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  * AND THE UNIVERSITY OF WISCONSIN-MADISON "AS IS" AND ANY EXPRESS OR
  * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  * WARRANTIES OF MERCHANTABILITY, OF SATISFACTORY QUALITY, AND FITNESS
  * FOR A PARTICULAR PURPOSE OR USE ARE DISCLAIMED. THE COPYRIGHT
  * HOLDERS AND CONTRIBUTORS AND THE UNIVERSITY OF WISCONSIN-MADISON
  * MAKE NO MAKE NO REPRESENTATION THAT THE SOFTWARE, MODIFICATIONS,
  * ENHANCEMENTS OR DERIVATIVE WORKS THEREOF, WILL NOT INFRINGE ANY
  * PATENT, COPYRIGHT, TRADEMARK, TRADE SECRET OR OTHER PROPRIETARY
  * RIGHT.
  *
  ****************************Copyright-DO-NOT-REMOVE-THIS-LINE**/
#include "condor_common.h"

/* 
	This file implementes blocking read/write operations on regular
	files. The main purpose is to try very hard to read/write however
	many bytes you actually specified. However, if there is an error,
	it is undefined how many bytes were actually read of written
	and where the file offset is. These functions soak EINTR.

	These functions are name mangled because they can be used in the
	checkpointing and remote i/o libraries. Use the nicely named functions
	if you are authoring code not in the above libraries.
*/

ssize_t
_condor_full_read(int fd, void *ptr, size_t nbytes)
{
	int nleft, nread;

	nleft = nbytes;
	while (nleft > 0) {

#ifndef WIN32
		REISSUE_READ: 
#endif
		nread = read(fd, ptr, nleft);
		if (nread < 0) {

#ifndef WIN32
			/* error happened, ignore if EINTR, otherwise inform the caller */
			if (errno == EINTR) {
				goto REISSUE_READ;
			}
#endif
			/* The caller has no idea how much was actually read in this
				scenario and the file offset is undefined */
			return -1;

		} else if (nread == 0) {
			/* We've reached the end of file marker, so stop looping. */
			break;
		}

		nleft -= nread;
			/* On Win32, void* does not default to "byte", so we cast it */
		ptr = ((char *)ptr) + nread;
	}

	/* return how much was actually read, which could include 0 in an
		EOF situation */
	return (nbytes - nleft);	 
}

ssize_t
_condor_full_write(int fd, const void *ptr, size_t nbytes)
{
	int nleft, nwritten;

	nleft = nbytes;
	while (nleft > 0) {
#ifndef WIN32
		REISSUE_WRITE: 
#endif
		nwritten = write(fd, ptr, nleft);
		if (nwritten < 0) {
#ifndef WIN32
			/* error happened, ignore if EINTR, otherwise inform the caller */
			if (errno == EINTR) {
				goto REISSUE_WRITE;
			}
#endif
			/* The caller has no idea how much was written in this scenario
				and the file offset is undefined. */
			return -1;
		}

		nleft -= nwritten;
			/* On Win32, void* does not default to "byte", so we cast it */
		ptr   = ((char*) ptr) + nwritten;
	}
	
	/* return how much was actually written, which could include 0 */
	return (nbytes - nleft);
}


