dist_pkgdata_DATA       = syncevo-http-trayicon.glade   \
                          syncevo-http-ui.png           \
                          sync-running.png              \
                          sync-stopped.png

pkgpython_PYTHON        = syncevo_http_trayicon.py
dist_bin_SCRIPTS        = syncevo-http-ui

desktopfilesdir         = $(datadir)/applications
dist_desktopfiles_DATA  = syncevo-http-ui.desktop

install-data-hook:
	sed  -i 's|Icon=.|Icon=$(DESTDIR)$(pkgdatadir)|'                \
            $(DESTDIR)$(desktopfilesdir)/syncevo-http-ui.desktop
	desktop-file-validate                                           \
            $(DESTDIR)$(desktopfilesdir)/syncevo-http-ui.desktop
	-update-desktop-database
	sed -i 's|^DATADIR[ ]*=.*|DATADIR = "$(DESTDIR)$(pkgdatadir)"|' \
            $(DESTDIR)$(pkgpythondir)/syncevo_http_trayicon.py
	rm -f $(DESTDIR)$(pkgpythondir)/*.pyo

uninstall-hook:
	update-desktop-database
