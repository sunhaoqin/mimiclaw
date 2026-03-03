#!/bin/bash
source /opt/devcontainer/claude-code_post_create.sh

echo 'source ${IDF_PATH}/export.sh' >> ~/.bashrc

echo 'source ${IDF_PATH}/export.sh' >> ~/.zshrc

echo 'The ownership of the /opt/esp directory is being changed.'
echo 'There are a large number of files, so please be patient and wait...'
if [ -d "/opt/esp" ]; then
    find /opt/esp -print0 | sudo xargs -0 -r -P $(nproc) chown ${CURRENT_USER}:${CURRENT_GROUP}
fi