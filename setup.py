from setuptools import setup, find_packages

setup(
    name='gwbasickernel',
    version='0.1.0',
    description='Jupyter kernel for GW-BASIC 2026',
    author='Eremey Valetov',
    url='https://github.com/evvaletov/gw-basic-2026',
    packages=find_packages(),
    install_requires=['jupyter_client', 'ipykernel'],
    entry_points={
        'console_scripts': [
            'gwbasickernel-install = gwbasickernel.install:main',
        ],
        'pygments.lexers': [
            'gwbasic = gwbasickernel.basic_lexer:GWBasicLexer',
        ],
    },
    package_data={
        'gwbasickernel': ['kernel.json'],
    },
    classifiers=[
        'Framework :: Jupyter',
        'License :: OSI Approved :: MIT License',
        'Programming Language :: Python :: 3',
    ],
)
