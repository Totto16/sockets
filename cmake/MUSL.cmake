function(check_musl target_name)
    if(NOT WIN32)
        check_c_source_compiles("#include <netinet/in.h>
int main(void) {
    struct sockaddr_in6* ipv6_info = 0;
    ipv6_info->sin6_addr.__in6_u.__u6_addr8;
}" HAVE_SOCKETS_LIBC)

        if(NOT HAVE_SOCKETS_LIBC)
            target_compile_definitions(${target_name} PRIVATE SOCKETS_USE_MUSL)
        endif()
    endif()
endfunction()
