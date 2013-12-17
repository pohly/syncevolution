#
# Hand-written Makefile for using installed tests. Installed together
# with test binaries and data files as part of "make install" if
# either --enable-unit-tests or --enable-integration-tests was used
# during configure.
#
# Currently it only provides the "testclean" target
# expected by runtest.py.
#

.PHONY: testclean clean

testclean:
	rm -rf *.test.vcf *.log *.log.html *.tests *.diff *.dat *Client_Sync_*client.* *Client_Source*

clean: testclean
