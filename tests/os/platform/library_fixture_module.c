/* The loader must resolve this module's sibling dependency. */
int jce_library_dependency_value(void);
int jce_library_fixture_value(void);
int jce_library_fixture_value(void) { return jce_library_dependency_value() + 5; }
