IF (HOST_OS_DARWIN AND HOST_ARCH_X86_64)
    DECLARE_EXTERNAL_RESOURCE(TEST_TOOL_HOST sbr:8895832159)
ELSEIF (HOST_OS_DARWIN AND HOST_ARCH_ARM64)
    DECLARE_EXTERNAL_RESOURCE(TEST_TOOL_HOST sbr:8895829195)
ELSEIF (HOST_OS_LINUX AND HOST_ARCH_X86_64)
    DECLARE_EXTERNAL_RESOURCE(TEST_TOOL_HOST sbr:8895837699)
ELSEIF (HOST_OS_LINUX AND HOST_ARCH_AARCH64)
    DECLARE_EXTERNAL_RESOURCE(TEST_TOOL_HOST sbr:8895827132)
ELSEIF (HOST_OS_WINDOWS AND HOST_ARCH_X86_64)
    DECLARE_EXTERNAL_RESOURCE(TEST_TOOL_HOST sbr:8895835134)

ENDIF()
