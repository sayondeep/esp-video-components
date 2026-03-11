set(srcs
    "push_av_stream_main.c"
    "cmaf_mux.c"
    "cmaf_push.c"
    "mpd_gen.c"
)

# server_root_ca.pem is ALWAYS embedded for TLS server certificate verification.
# client_cert.pem / client_key.pem are only needed when mTLS is enabled.
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

idf_component_register(
    SRCS            ${srcs}
    INCLUDE_DIRS    "."
    EMBED_TXTFILES  ${embed_files}
)
