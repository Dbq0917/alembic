#include <Alembic/Ogawa/All.h>

#include <cstdio>

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: abcvalidate <archive> [archive ...]\n");
        return 2;
    }

    int result = 0;
    for (int i = 1; i < argc; ++i)
    {
        Alembic::Ogawa::ValidationResult validation =
            Alembic::Ogawa::ValidateArchive(argv[i]);
        if (!validation.valid)
        {
            std::fprintf(stderr, "%s: INVALID: %s\n", argv[i],
                         validation.error.c_str());
            result = 1;
        }
        else if (validation.recovered)
        {
            std::printf("%s: OK (recovered to last committed state)\n",
                        argv[i]);
        }
        else
        {
            std::printf("%s: OK\n", argv[i]);
        }
    }
    return result;
}
