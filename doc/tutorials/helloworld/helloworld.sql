CREATE TABLE HELLOWORLD (
   HELLO VARCHAR(15),
   WORLD VARCHAR(15),
   DIALECT VARCHAR(15) NOT NULL,
   PRIMARY KEY (DIALECT)
);

PARTITION TABLE HELLOWORLD ON COLUMN DIALECT;

LOAD CLASSES helloworld.jar;

CREATE PROCEDURE PARTITION ON TABLE Helloworld COLUMN Dialect FROM CLASS Insert;
CREATE PROCEDURE PARTITION ON TABLE Helloworld COLUMN Dialect FROM CLASS Select;
