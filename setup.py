#!/usr/bin/env python3

from setuptools import setup
from setuptools import find_packages


with open("README.md", "r", encoding="utf-8") as fp:
    long_description = fp.read()


setup(
    name                          = "litex_m2sdr",
    description                   = "LiteX M2 SDR",
    long_description              = long_description,
    long_description_content_type = "text/markdown",
    author                        = "Enjoy-Digital",
    author_email                  = "florent@enjoy-digital.fr",
    url                           = "http://enjoy-digital.fr",
    download_url                  = "https://github.com/enjoy-digital/litex_m2sdr",
    license                       = "BSD",
    python_requires               = "~=3.7",
    install_requires              = ["litex"],
    include_package_data          = True,
    keywords                      = "HDL ASIC FPGA hardware design",
    classifiers                   = [
        "Topic :: Scientific/Engineering :: Electronic Design Automation (EDA)",
        "Environment :: Console",
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "License :: OSI Approved :: BSD License",
        "Operating System :: OS Independent",
        "Programming Language :: Python",
    ],
    packages                      = find_packages(exclude=['test*']),
    py_modules                    = ['litex_m2sdr_platform'],
)
