#!/usr/bin/with-contenv bashio
set -e

DATA_SUBPATH=$(bashio::config 'data_subpath')
LOG_LEVEL=$(bashio::config 'log_level')

mkdir -p "/share/${DATA_SUBPATH}"

export MWW_CAPTURE_DATA_DIR="/share/${DATA_SUBPATH}"
export MWW_CAPTURE_HOST="0.0.0.0"
export MWW_CAPTURE_PORT="8765"

bashio::log.info "MWW Training Capture starting"
bashio::log.info "Data dir: ${MWW_CAPTURE_DATA_DIR}"
bashio::log.info "Listening on :${MWW_CAPTURE_PORT}"

cd /opt/mww
exec /opt/mww/venv/bin/python -m service
