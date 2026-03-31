CREATE TABLE IF NOT EXISTS devices (
    device_id         VARCHAR(20)  PRIMARY KEY,
    mac_address       VARCHAR(17)  NOT NULL,
    firmware_version  VARCHAR(20)  NOT NULL,
    hardware_revision VARCHAR(10)  NOT NULL,
    status            VARCHAR(20)  NOT NULL DEFAULT 'active'
                      CHECK (status IN ('active', 'inactive')),
    first_seen_at     TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    last_seen_at      TIMESTAMPTZ  NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS device_configs (
    id                     SERIAL       PRIMARY KEY,
    device_id              VARCHAR(20)  NOT NULL REFERENCES devices(device_id),
    config_id              VARCHAR(50)  NOT NULL UNIQUE,
    port                   SMALLINT     NOT NULL CHECK (port IN (1, 2)),
    watering_schedule      JSONB        NOT NULL,
    humidity_threshold_pct INTEGER      NOT NULL CHECK (humidity_threshold_pct BETWEEN 0 AND 100),
    status                 VARCHAR(20)  NOT NULL DEFAULT 'pending'
                           CHECK (status IN ('pending', 'pushed', 'applied', 'rejected')),
    created_at             TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    pushed_at              TIMESTAMPTZ,
    acked_at               TIMESTAMPTZ
);

CREATE INDEX IF NOT EXISTS idx_device_configs_device_port
    ON device_configs(device_id, port);

CREATE TABLE IF NOT EXISTS watering_events (
    id                    BIGSERIAL    PRIMARY KEY,
    device_id             VARCHAR(20)  NOT NULL REFERENCES devices(device_id),
    port                  SMALLINT     NOT NULL CHECK (port IN (1, 2)),
    event_type            VARCHAR(20)  NOT NULL
                          CHECK (event_type IN ('watering_started', 'watering_stopped')),
    device_timestamp      TIMESTAMPTZ  NOT NULL,
    received_at           TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    trigger               VARCHAR(20)  NOT NULL
                          CHECK (trigger IN ('schedule', 'humidity')),
    scheduled_time        TIME,
    humidity_at_start_pct NUMERIC(5,2),
    humidity_at_stop_pct  NUMERIC(5,2),
    planned_duration_s    INTEGER,
    actual_duration_s     INTEGER,
    stop_reason           VARCHAR(30)
                          CHECK (stop_reason IS NULL OR stop_reason IN ('duration_complete', 'error'))
);

CREATE INDEX IF NOT EXISTS idx_watering_events_device_port_ts
    ON watering_events(device_id, port, device_timestamp DESC);
