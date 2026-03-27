set(srcs
    "push_av_stream_main.c"
    "cmaf_mux.c"
    "mpd_gen.c"
    "ingest_transport.c"
    "ingest_adapters/matter_adapter.c"
    "ingest_adapters/nagare_adapter.c"
)

# TLS cert files are only needed for the Matter backend (HTTPS).
# nagare-media/ingest uses plain HTTP — no certs required.
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
else()
    set(embed_files "")
endif()

idf_component_register(
    SRCS            ${srcs}
    INCLUDE_DIRS    "."
    EMBED_TXTFILES  ${embed_files}
)
