##############################################################
#
# AESDCHAR
#
##############################################################

#TODO: Fill up the contents below in order to reference your assignment 3 git contents
#AESDCHAR_VERSION = '2f4d1de5cdc207270a954c78639d9ec10bcd22e8'
AESDCHAR_VERSION = 1.0
# Note: Be sure to reference the *ssh* repository URL here (not https) to work properly
# with ssh keys and the automated build/test system.
# Your site should start with git@github.com:
#AESDCHAR_SITE = 'git@github.com:NMT81/aeld-assignment-3-and-later.git'
#AESDCHAR_SITE_METHOD = git
#AESDCHAR_GIT_SUBMODULES = YES
AESDCHAR_SITE = $(BR2_EXTERNAL_project_base_PATH)/package/aesdchar/src
AESDCHAR_SITE_METHOD = local

#AESDCHAR_MODULE_SUBDIRS = aesd-char-driver
define AESDCHAR_INSTALL_EXTRA_FILES
	$(INSTALL) -m 0755 $(@D)/aesdchar-start-stop $(TARGET_DIR)/etc/init.d/S97aesdchar
endef
#define AESDCHAR_INSTALL_EXTRA_FILES
#	$(INSTALL) -m 0755 $(@D)/aesd-char-driver/aesdchar-start-stop $(TARGET_DIR)/etc/init.d/S97aesdchar
#endef
AESDCHAR_POST_INSTALL_TARGET_HOOKS += AESDCHAR_INSTALL_EXTRA_FILES

$(eval $(kernel-module))
$(eval $(generic-package))