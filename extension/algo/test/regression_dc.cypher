LOAD EXTENSION '/Users/qingchen/projects/Dynamic-Connectivity-Index-for-Kuzu/extension/algo/build/libalgo.kuzu_extension';
CREATE NODE TABLE Person(id INT64, PRIMARY KEY(id));
CREATE REL TABLE Knows(FROM Person TO Person);
CREATE (:Person {id: 0});
CREATE (:Person {id: 1});
CREATE (:Person {id: 2});
CREATE (:Person {id: 3});
MATCH (a:Person {id: 1}), (b:Person {id: 2}) CREATE (a)-[:Knows]->(b);

CALL CREATE_DYNAMIC_CONNECTIVITY_INDEX('Person', 'Knows', 'dc_knows', 'stree') RETURN *;
CALL DYNAMIC_CONNECTIVITY_QUERY('Person', 1, 2, 'dc_knows') RETURN *;
CALL DYNAMIC_CONNECTIVITY_QUERY('Person', 1, 3, 'dc_knows') RETURN *;

MATCH (a:Person {id: 2}), (b:Person {id: 3}) CREATE (a)-[:Knows]->(b);
CALL DYNAMIC_CONNECTIVITY_QUERY('Person', 1, 3, 'dc_knows') RETURN *;

CALL DYNAMIC_CONNECTIVITY_DELETE_EDGE('Person', 'dc_knows', 2, 3) RETURN *;
CALL DYNAMIC_CONNECTIVITY_QUERY('Person', 1, 3, 'dc_knows') RETURN *;
CALL DYNAMIC_CONNECTIVITY_QUERY('Person', 1, 2, 'dc_knows') RETURN *;

CALL DYNAMIC_CONNECTIVITY_QUERY('Person', 0, 1, 'dc_knows') RETURN *;
BEGIN TRANSACTION;
MATCH (a:Person {id: 0}), (b:Person {id: 1}) CREATE (a)-[:Knows]->(b);
ROLLBACK;
CALL DYNAMIC_CONNECTIVITY_QUERY('Person', 0, 1, 'dc_knows') RETURN *;

BEGIN TRANSACTION;
MATCH (a:Person {id: 0}), (b:Person {id: 1}) CREATE (a)-[:Knows]->(b);
MATCH (a:Person {id: 0})-[k:Knows]->(b:Person {id: 1}) DELETE k;
COMMIT;
CALL DYNAMIC_CONNECTIVITY_QUERY('Person', 0, 1, 'dc_knows') RETURN *;
MATCH (:Person {id: 0})-[k:Knows]->(:Person {id: 1}) RETURN count(k);

MATCH (a:Person {id: 0}), (b:Person {id: 1}) CREATE (a)-[:Knows]->(b);
CALL DYNAMIC_CONNECTIVITY_QUERY('Person', 0, 1, 'dc_knows') RETURN *;
CALL DYNAMIC_CONNECTIVITY_QUERY('Person', 0, 2, 'dc_knows') RETURN *;
CALL DYNAMIC_CONNECTIVITY_QUERY('Person', 0, 3, 'dc_knows') RETURN *;
