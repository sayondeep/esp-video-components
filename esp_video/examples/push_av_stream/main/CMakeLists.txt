set(srcs
    "push_av_stream_main.c"
    "cmaf_mux.c"
    "mpd_gen.c"
    "ingest_transport.c"
    "ingest_adapters/matter_adapter.c"
    "ingest_adapters/nagare_adapter.c"
)

# Embed TLS certificate files based on the selected server backend and TLS options.
#
# Matter backend:  always uses HTTPS; optionally mTLS with client cert+key.
# Nagare backend:  plain HTTP by default; HTTPS via nginx proxy when
#                  EXAMPLE_NAGARE_USE_TLS is enabled (requires the nginx proxy's
#                  server CA cert at main/certs/nagare_server_ca.pem).
if(CONFIG_EXAMPLE_SERVER_TYPE_MATTER)
    if(CONFIG_EXAMPLE_USE_MTLS)
        set(embed_files
            "certs/server_root_ca.pem"
            "certs/client_cert.pem"
            "certs/client_key.pem"
        )
    else()
        set(embed_files
            "certs/server_root_ca.pem"
        )
    endif()
elseif(CONFIG_EXAMPLE_NAGARE_USE_TLS)
    # Server CA cert for the nginx TLS proxy.
    # Generate with:  cd docker/certs && ./gen_cert.sh <LAN_IP>
    # Then copy:      cp docker/certs/server.crt main/certs/nagare_server_ca.pem
    set(embed_files
        "certs/nagare_server_ca.pem"
    )
else()
    set(embed_files "")
endif()

idf_component_register(
    SRCS            ${srcs}
    INCLUDE_DIRS    "."
    EMBED_TXTFILES  ${embed_files}
)
